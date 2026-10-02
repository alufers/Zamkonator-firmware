#include "event_manager.h"
#include "mqtt.h"
#include "sdcard.h"
#include "utils.h"
#include "webserver.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "event_manager";

#define LOG_DIR          SDCARD_MOUNT_POINT "/logs"
#define EVENT_QUEUE_LEN  16
#define EVENT_RING_MAX   64      /* hard cap on entry count */
#define EVENT_RING_BYTES 5120    /* soft cap on total JSON bytes */

/* ── Type names ──────────────────────────────────────────────────────────── */

/* Indexed by enum event_payload_t_tag; must track schema_events.json-gen-c. */
static const char *const k_type_names[] = {
    [event_payload_t_booted]             = "booted",
    [event_payload_t_coredump_saved]     = "coredump_saved",
    [event_payload_t_card_scanned]       = "card_scanned",
    [event_payload_t_door_state_changed] = "door_state_changed",
    [event_payload_t_lock_state_changed] = "lock_state_changed",
    [event_payload_t_push_to_exit]       = "push_to_exit",
    [event_payload_t_remote_open]        = "remote_open",
};

const char *event_manager_type_name(int payload_tag)
{
    if (payload_tag < 0 ||
        payload_tag >= (int)(sizeof(k_type_names) / sizeof(k_type_names[0])))
        return "unknown";
    return k_type_names[payload_tag] ? k_type_names[payload_tag] : "unknown";
}

/* ── Queue item ──────────────────────────────────────────────────────────── */

typedef struct {
    char *json;       /* malloc'd, NUL-terminated; owned by the fan-out task */
    size_t len;
    int    tag;       /* enum event_payload_t_tag, for the MQTT topic */
} event_item_t;

/* ── Ring buffer ─────────────────────────────────────────────────────────── */

typedef struct {
    char  *json;
    size_t len;
    bool   logged;    /* already written to the SD log file */
} ring_entry_t;

static ring_entry_t     s_ring[EVENT_RING_MAX];
static int              s_ring_head  = 0;   /* index of the oldest entry */
static int              s_ring_count = 0;
static size_t           s_ring_bytes = 0;
static SemaphoreHandle_t s_ring_mutex;

static QueueHandle_t s_queue;
static int64_t       s_next_seq = 1;

/* Caller must hold s_ring_mutex. */
static void ring_evict_oldest_locked(void)
{
    if (s_ring_count == 0) return;
    ring_entry_t *e = &s_ring[s_ring_head];
    s_ring_bytes -= e->len;
    free(e->json);
    e->json = NULL;
    s_ring_head = (s_ring_head + 1) % EVENT_RING_MAX;
    s_ring_count--;
}

/* Takes ownership of @p json. */
static void ring_push(char *json, size_t len, bool logged)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);

    while (s_ring_count >= EVENT_RING_MAX ||
           (s_ring_count > 0 && s_ring_bytes + len > EVENT_RING_BYTES))
        ring_evict_oldest_locked();

    int idx = (s_ring_head + s_ring_count) % EVENT_RING_MAX;
    s_ring[idx].json   = json;
    s_ring[idx].len    = len;
    s_ring[idx].logged = logged;
    s_ring_count++;
    s_ring_bytes += len;

    xSemaphoreGive(s_ring_mutex);
}

sstr_t event_manager_ring_json(void)
{
    sstr_t out = sstr_new();
    sstr_append_cstr(out, "[");

    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    for (int i = 0; i < s_ring_count; i++) {
        const ring_entry_t *e = &s_ring[(s_ring_head + i) % EVENT_RING_MAX];
        if (i) sstr_append_cstr(out, ",");
        sstr_append_of(out, e->json, e->len);
    }
    xSemaphoreGive(s_ring_mutex);

    sstr_append_cstr(out, "]");
    return out;
}

/* ── SD log file ─────────────────────────────────────────────────────────── */

static bool sd_log_available(void)
{
    return sdcard_is_mounted() && utils_time_is_valid();
}

/* Append one JSON line to logs/<today>.log. Returns true on success. */
static bool sd_log_write(const char *json, size_t len)
{
    if (mkdir(LOG_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGW(TAG, "Cannot create %s: %d", LOG_DIR, errno);
        return false;
    }

    char path[64];
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    strftime(path, sizeof(path), LOG_DIR "/%Y-%m-%d.log", &tm_info);

    FILE *f = fopen(path, "a");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for append: %d", path, errno);
        return false;
    }

    bool ok = fwrite(json, 1, len, f) == len && fputc('\n', f) != EOF;
    fclose(f);

    if (!ok) ESP_LOGW(TAG, "Write to %s failed", path);
    return ok;
}

/* Write every ring entry that has not reached the SD card yet, oldest first.
 * Runs when the clock becomes valid or the card is (re)mounted, so events
 * buffered before either was available still get persisted. */
static void sd_log_flush_backlog(void)
{
    for (int i = 0; i < EVENT_RING_MAX; i++) {
        char  *json = NULL;
        size_t len  = 0;
        int    idx  = -1;

        /* Copy one pending entry out under the lock — sd_log_write() does
         * blocking file I/O and must not hold up /api/events. */
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        for (int j = 0; j < s_ring_count; j++) {
            ring_entry_t *e = &s_ring[(s_ring_head + j) % EVENT_RING_MAX];
            if (!e->logged) {
                idx  = (s_ring_head + j) % EVENT_RING_MAX;
                len  = e->len;
                json = malloc(len + 1);
                if (json) memcpy(json, e->json, len + 1);
                break;
            }
        }
        xSemaphoreGive(s_ring_mutex);

        if (idx < 0 || !json) {
            free(json);
            return;
        }

        bool ok = sd_log_write(json, len);
        free(json);
        if (!ok) return;

        /* Mark logged — unless eviction recycled the slot while we were
         * writing, in which case the entry is gone and there is nothing to
         * mark. Comparing the length is enough to catch the common case. */
        xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
        if (s_ring[idx].json && s_ring[idx].len == len)
            s_ring[idx].logged = true;
        xSemaphoreGive(s_ring_mutex);
    }
}

/* ── Fan-out task ────────────────────────────────────────────────────────── */

static void event_broadcast_ws(const char *json, size_t len)
{
    /* Wrap the marshalled event in the ws_server_message_t envelope by hand:
     * the generated marshaller would have to re-serialise the whole payload. */
    static const char PRE[]  = "{\"cmd\":\"event\",\"payload\":";
    static const char POST[] = "}";

    char *buf = malloc(sizeof(PRE) - 1 + len + sizeof(POST));
    if (!buf) return;
    memcpy(buf, PRE, sizeof(PRE) - 1);
    memcpy(buf + sizeof(PRE) - 1, json, len);
    memcpy(buf + sizeof(PRE) - 1 + len, POST, sizeof(POST));

    webserver_ws_broadcast_json(buf);
    free(buf);
}

static void event_manager_task(void *arg)
{
    (void)arg;

    for (;;) {
        event_item_t item;
        if (xQueueReceive(s_queue, &item, portMAX_DELAY) != pdTRUE)
            continue;

        /* 1. SD card — flush anything still pending first so the file keeps
         *    the same order the events were emitted in. */
        bool logged = false;
        if (sd_log_available()) {
            sd_log_flush_backlog();
            logged = sd_log_write(item.json, item.len);
        }

        /* 2. WebSocket */
        event_broadcast_ws(item.json, item.len);

        /* 3. MQTT */
        mqtt_publish_event(event_manager_type_name(item.tag), item.json);

        /* 4. In-memory ring (takes ownership of item.json) */
        ring_push(item.json, item.len, logged);
    }
}

/* ── Submission ──────────────────────────────────────────────────────────── */

void event_manager_submit(struct event_payload_t *payload)
{
    struct event_t ev;
    event_t_init(&ev);
    ev.uptime  = esp_timer_get_time() / 1000000LL;
    ev.payload = *payload;

    if (utils_time_is_valid()) {
        ev.has_time = 1;
        ev.time     = (int64_t)time(NULL);
    }

    /* Sequence numbers are handed out at marshal time so the JSON order in the
     * ring, the log file and the WebSocket all agree. */
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    ev.seq = s_next_seq++;
    xSemaphoreGive(s_ring_mutex);

    sstr_t json = sstr_new();
    json_marshal_event_t(&ev, json);

    event_item_t item = {
        .len = sstr_length(json),
        .tag = (int)payload->tag,
    };
    item.json = malloc(item.len + 1);
    if (item.json)
        memcpy(item.json, sstr_cstr(json), item.len + 1);

    sstr_free(json);

    /* ev.payload aliases *payload; clearing ev frees it exactly once. */
    event_t_clear(&ev);
    memset(payload, 0, sizeof(*payload));

    if (!item.json) return;

    if (xQueueSend(s_queue, &item, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Event queue full, dropping %s",
                 event_manager_type_name(item.tag));
        free(item.json);
    }
}

/* ── Emitters ────────────────────────────────────────────────────────────── */

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

void event_emit_booted(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();

    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_booted;
    p.value.booted.firmware_version = sstr(desc ? desc->version : "unknown");
    p.value.booted.reset_reason     = sstr(reset_reason_str());
    event_manager_submit(&p);
}

void event_emit_coredump_saved(const char *path, long size)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_coredump_saved;
    p.value.coredump_saved.path = sstr(path);
    p.value.coredump_saved.size = (int64_t)size;
    event_manager_submit(&p);
}

void event_emit_card_scanned(const struct ev_card_scanned_t *scan)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_card_scanned;
    p.value.card_scanned = *scan;   /* takes ownership of the sstr_t members */
    event_manager_submit(&p);
}

void event_emit_door_state(bool closed)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_door_state_changed;
    p.value.door_state_changed.closed = closed;
    event_manager_submit(&p);
}

void event_emit_lock_state(bool locked)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_lock_state_changed;
    p.value.lock_state_changed.locked = locked;
    event_manager_submit(&p);
}

void event_emit_push_to_exit(int input_idx)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_push_to_exit;
    p.value.push_to_exit.input = input_idx;
    event_manager_submit(&p);
}

void event_emit_remote_open(const char *reason, int open_time_ms)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_remote_open;
    if (reason && reason[0]) {
        p.value.remote_open.has_reason = 1;
        p.value.remote_open.reason     = sstr(reason);
    }
    p.value.remote_open.open_time_ms = open_time_ms;
    event_manager_submit(&p);
}

/* ── Init ────────────────────────────────────────────────────────────────── */

void event_manager_init(void)
{
    s_ring_mutex = xSemaphoreCreateMutex();
    s_queue      = xQueueCreate(EVENT_QUEUE_LEN, sizeof(event_item_t));
    xTaskCreate(event_manager_task, "event_mgr", 6144, NULL, 4, NULL);
    ESP_LOGI(TAG, "Event manager started (ring %d bytes / %d entries)",
             EVENT_RING_BYTES, EVENT_RING_MAX);
}
