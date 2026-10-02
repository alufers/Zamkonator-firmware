#include "mqtt.h"
#include "config.h"
#include "digital_inputs.h"
#include "webserver.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mqtt";

/* ── Global status ───────────────────────────────────────────────────────── */

int g_mqtt_status = mqtt_status_t_unconfigured;

/* ── Internal state ──────────────────────────────────────────────────────── */

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t        s_mutex;

typedef struct {
    bool  enabled;
    char  broker[128];
    int   port;
    char  username[64];
    char  password[128];
    char  prefix[64];
    char  hostname[64];
} mqtt_active_cfg_t;

static mqtt_active_cfg_t s_active_cfg;

/* "<prefix>/<hostname>" — recomputed whenever the client is (re)initialised.
 * Published topics are built as "<s_topic_base>/<subtopic>". */
static char s_topic_base[132];
static char s_avty_topic[152];

/* ── MQTT event handler ──────────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        g_mqtt_status = mqtt_status_t_connected;
        mqtt_publish("availability", "online", 0, true);
        /* Re-publish retained input state so a reconnecting broker (or one
         * that lost its retained set) is resynced without waiting for the
         * next physical edge. */
        digital_inputs_publish_all_states();
        webserver_push_status();
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected from broker");
        g_mqtt_status = mqtt_status_t_disconnected;
        webserver_push_status();
        break;

    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
        ESP_LOGW(TAG, "MQTT error: type=%d", (int)ev->error_handle->error_type);
        break;
    }

    default:
        break;
    }
}

/* ── Init / deinit helpers ───────────────────────────────────────────────── */

static void mqtt_do_deinit(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
    s_topic_base[0] = '\0';
    s_avty_topic[0] = '\0';
}

static void mqtt_do_init(const mqtt_active_cfg_t *cfg)
{
    if (!cfg->enabled || cfg->broker[0] == '\0') {
        g_mqtt_status = mqtt_status_t_unconfigured;
        return;
    }

    char uri[192];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->broker, cfg->port);

    snprintf(s_topic_base, sizeof(s_topic_base), "%s/%s",
             cfg->prefix, cfg->hostname);
    snprintf(s_avty_topic, sizeof(s_avty_topic), "%s/availability",
             s_topic_base);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri  = uri,
        .session.keepalive   = 60,
        /* The CONNECTED handler runs on this task and now does a status
         * marshal plus the input-state resync, so it needs the headroom. */
        .task.stack_size     = 8192,
        .buffer.size         = 2048,
        /* Broker publishes this if we drop off without a clean disconnect. */
        .session.last_will = {
            .topic  = s_avty_topic,
            .msg    = "offline",
            .msg_len = (int)strlen("offline"),
            .qos    = 0,
            .retain = 1,
        },
    };

    if (cfg->username[0]) {
        mqtt_cfg.credentials.username                = cfg->username;
        mqtt_cfg.credentials.authentication.password = cfg->password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        g_mqtt_status = mqtt_status_t_disconnected;
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    g_mqtt_status = mqtt_status_t_connecting;
    s_active_cfg  = *cfg;
    ESP_LOGI(TAG, "MQTT client started → %s", uri);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

/* prefix and hostname are part of the snapshot because they determine the LWT
 * topic, which can only be set while the client is being created — so a change
 * to either has to force a reconnect. */
static void snapshot_config(mqtt_active_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    config_lock();
    cfg->enabled = (bool)g_config.mqtt.enabled;
    cfg->port    = g_config.mqtt.port;
    strlcpy(cfg->broker,   sstr_cstr(g_config.mqtt.broker),      sizeof(cfg->broker));
    strlcpy(cfg->username, sstr_cstr(g_config.mqtt.username),    sizeof(cfg->username));
    strlcpy(cfg->password, sstr_cstr(g_config.mqtt.password),    sizeof(cfg->password));
    strlcpy(cfg->prefix,   sstr_cstr(g_config.mqtt.mqtt_prefix), sizeof(cfg->prefix));
    strlcpy(cfg->hostname, sstr_cstr(g_config.hostname),         sizeof(cfg->hostname));
    config_unlock();
}

void mqtt_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));

    mqtt_active_cfg_t cfg;
    snapshot_config(&cfg);
    mqtt_do_init(&cfg);
}

void mqtt_apply_config(void)
{
    mqtt_active_cfg_t cfg;
    snapshot_config(&cfg);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (memcmp(&s_active_cfg, &cfg, sizeof(cfg)) != 0);
    if (changed) {
        ESP_LOGI(TAG, "MQTT config changed, reinitialising");
        mqtt_do_deinit();
        mqtt_do_init(&cfg);
    }
    xSemaphoreGive(s_mutex);
}

/* ── Publishing ──────────────────────────────────────────────────────────── */

bool mqtt_publish(const char *subtopic, const char *payload, int qos, bool retain)
{
    if (!s_client || g_mqtt_status != mqtt_status_t_connected) return false;
    if (s_topic_base[0] == '\0') return false;

    char topic[192];
    snprintf(topic, sizeof(topic), "%s/%s", s_topic_base, subtopic);

    int msg_id = esp_mqtt_client_publish(s_client, topic, payload,
                                          0, qos, retain ? 1 : 0);
    if (msg_id < 0) {
        ESP_LOGW(TAG, "Publish to %s failed", topic);
        return false;
    }
    return true;
}

void mqtt_publish_event(const char *type_name, const char *json)
{
    char subtopic[96];
    snprintf(subtopic, sizeof(subtopic), "event/%s", type_name);
    mqtt_publish(subtopic, json, 0, false);
}
