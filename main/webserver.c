#include "webserver.h"
#include "auth_proxy.h"
#include "ota.h"
#include "background_worker.h"
#include "config.h"
#include "digital_inputs.h"
#include "ethernet_manager.h"
#include "event_manager.h"
#include "mqtt.h"
#include "outputs.h"
#include "sdcard.h"
#include "utils.h"

#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_system.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mdns.h"

static const char *TAG = "webserver";

extern int g_mqtt_status; /* enum mqtt_status_t */

/* ── Config ──────────────────────────────────────────────────────────────── */

#define MAX_WS_CLIENTS      4
#define STATUS_HEARTBEAT_US (10ULL * 1000 * 1000)

/* ── State ───────────────────────────────────────────────────────────────── */

static httpd_handle_t    s_server = NULL;
static int               s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex;

/* ── Auth ────────────────────────────────────────────────────────────────── */

static bool check_auth(httpd_req_t *req)
{
    config_lock();
    bool enabled = g_config.web_password_enabled;
    config_unlock();
    if (!enabled) return true;

    char pw[128] = {0};
    if (httpd_req_get_hdr_value_str(req, "X-Auth", pw, sizeof(pw)) != ESP_OK)
        return false;

    config_lock();
    bool ok = utils_crypto_verify_password(pw, sstr_cstr(g_config.web_password));
    config_unlock();
    return ok;
}

#define REQUIRE_AUTH(req)                                                     \
    do {                                                                      \
        if (!check_auth(req)) {                                               \
            httpd_resp_set_hdr(req, "WWW-Authenticate", "X-Auth");            \
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized"); \
            return ESP_OK;                                                    \
        }                                                                     \
    } while (0)

/* ── Embedded file handlers ──────────────────────────────────────────────── */

#define STATIC_FILE_HANDLER(sym, ctype)                                          \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start");           \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end");             \
    static esp_err_t sym##_handler(httpd_req_t *req)                            \
    {                                                                            \
        httpd_resp_set_type(req, ctype);                                         \
        httpd_resp_send(req, (const char *)sym##_start,                          \
                        sym##_end - sym##_start);                                \
        return ESP_OK;                                                           \
    }

STATIC_FILE_HANDLER(index_html, "text/html")
STATIC_FILE_HANDLER(app_js,    "application/javascript")
STATIC_FILE_HANDLER(app_css,   "text/css")

/* ── WebSocket async-send helpers ────────────────────────────────────────── */

typedef struct {
    httpd_handle_t hd;
    int            fd;
    char           json[];
} ws_send_work_t;

static void ws_send_work(void *arg)
{
    ws_send_work_t *w = arg;
    httpd_ws_frame_t pkt = {
        .final      = true,
        .fragmented = false,
        .type       = HTTPD_WS_TYPE_TEXT,
        .payload    = (uint8_t *)w->json,
        .len        = strlen(w->json),
    };
    esp_err_t err = httpd_ws_send_frame_async(w->hd, w->fd, &pkt);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WS send fd=%d failed (%d), closing session", w->fd, err);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (s_ws_fds[i] == w->fd) s_ws_fds[i] = -1;
        xSemaphoreGive(s_ws_mutex);
        httpd_sess_trigger_close(w->hd, w->fd);
    }
    free(w);
}

static void ws_queue_send(int fd, const char *json)
{
    if (!s_server) return;
    size_t jlen = strlen(json);
    ws_send_work_t *w = malloc(sizeof(*w) + jlen + 1);
    if (!w) return;
    w->hd = s_server;
    w->fd = fd;
    memcpy(w->json, json, jlen + 1);
    if (httpd_queue_work(s_server, ws_send_work, w) != ESP_OK)
        free(w);
}

void webserver_ws_broadcast_json(const char *json)
{
    if (!s_server || !s_ws_mutex) return;

    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], json);
}

/* ── Status payload ──────────────────────────────────────────────────────── */

static void build_status_payload(struct ws_status_payload_t *p)
{
    ws_status_payload_t_init(p);
    p->uptime             = esp_timer_get_time() / 1000000LL;
    p->time               = (int64_t)time(NULL);
    p->mqtt_status        = g_mqtt_status;
    p->sd_card_mounted    = sdcard_is_mounted();
    p->auth_proxy_healthy = auth_proxy_is_healthy();

    bool sensor_val;
    if (digital_inputs_door_close_state(&sensor_val)) {
        p->has_door_close_sensor_closed = 1;
        p->door_close_sensor_closed     = sensor_val;
    }
    if (digital_inputs_door_lock_state(&sensor_val)) {
        p->has_door_lock_sensor_locked = 1;
        p->door_lock_sensor_locked     = sensor_val;
    }
}

/* fd == -1 broadcasts to every client. */
static void send_status(int fd)
{
    struct ws_server_message_t msg;
    ws_server_message_t_init(&msg);
    msg.tag = ws_server_message_t_status;
    build_status_payload(&msg.value.status.payload);

    sstr_t out = sstr_new();
    json_marshal_ws_server_message_t(&msg, out);
    ws_server_message_t_clear(&msg);

    if (fd == -1)
        webserver_ws_broadcast_json(sstr_cstr(out));
    else
        ws_queue_send(fd, sstr_cstr(out));
    sstr_free(out);
}

void webserver_push_status(void)
{
    send_status(-1);
}

static void status_timer_cb(void *arg)
{
    (void)arg;
    send_status(-1);
}

/* ── WebSocket handlers ──────────────────────────────────────────────────── */

static esp_err_t ws_pre_handshake_cb(httpd_req_t *req)
{
    config_lock();
    bool pw_enabled = g_config.web_password_enabled;
    config_unlock();
    if (!pw_enabled) return ESP_OK;

    /* Browsers cannot set headers on a WebSocket handshake, so the password
     * travels as a query parameter and is verified against the same PBKDF2
     * hash the REST endpoints use. */
    char query[256]    = {0};
    char auth_buf[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "auth", auth_buf, sizeof(auth_buf));

    config_lock();
    bool ok = utils_crypto_verify_password(auth_buf,
                                            sstr_cstr(g_config.web_password));
    config_unlock();

    if (!ok) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "X-Auth");
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_post_handshake_cb(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == -1) { slot = i; break; }
    }
    xSemaphoreGive(s_ws_mutex);

    if (slot < 0) {
        ESP_LOGW(TAG, "WS client list full, dropping fd=%d", fd);
        return ESP_OK;
    }

    /* Seed the new client before registering it, so the initial status is not
     * interleaved with a concurrent broadcast. */
    send_status(fd);

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_WS_CLIENTS; i++) {
        if (s_ws_fds[i] == -1) { s_ws_fds[i] = fd; break; }
    }
    xSemaphoreGive(s_ws_mutex);

    ESP_LOGI(TAG, "WS client connected fd=%d", fd);
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *buf = NULL;
    if (frame.len > 0) {
        buf = calloc(1, frame.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) {
            free(buf);
            return ret;
        }
    }

    /* The protocol is server→client only; inbound text frames are ignored. */
    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++)
            if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
        xSemaphoreGive(s_ws_mutex);
        ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
    }

    free(buf);
    return ret;
}

/* ── /api/status ─────────────────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    struct ws_status_payload_t p;
    build_status_payload(&p);
    p.has_network = true;
    ethernet_manager_get_status(&p.network);

    sstr_t json = sstr_new();
    json_marshal_indent_ws_status_payload_t(&p, 0, 0, json);
    ws_status_payload_t_clear(&p);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

/* ── /api/events ─────────────────────────────────────────────────────────── */

static esp_err_t events_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    sstr_t json = event_manager_ring_json();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

/* ── Settings REST handlers ──────────────────────────────────────────────── */

static esp_err_t send_settings_json(httpd_req_t *req)
{
    config_lock();
    sstr_t saved_pw       = g_config.web_password;
    g_config.web_password = sstr("");
    sstr_t json           = sstr_new();
    json_marshal_gateway_config_t(&g_config, json);
    sstr_free(g_config.web_password);
    g_config.web_password = saved_pw;
    config_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    return send_settings_json(req);
}

static void apply_settings_from_buf(const char *buf, int len)
{
    uint64_t mask[gateway_config_t_FIELD_MASK_WORD_COUNT] = {0};
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_hostname);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_mqtt);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_web_password_enabled);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_web_password);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_language);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_auth_proxy_base_url);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_auth_proxy_timeout_ms);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_auth_proxy_healthcheck_interval_ms);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_auth_proxy_cache_enabled);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_auth_proxy_cache_refresh_ms);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_relay_open_ms);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_input_inp1);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_input_inp2);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_input_inp3);
    JSON_GEN_C_FIELD_MASK_SET(mask, gateway_config_t_FIELD_remote_open_password);

    config_lock();
    sstr_t prev_pw = sstr_dup(g_config.web_password);
    config_unlock();

    sstr_t in = sstr_of(buf, (size_t)len);

    config_lock();
    json_unmarshal_selected_gateway_config_t(in, &g_config, mask,
                                              gateway_config_t_FIELD_MASK_WORD_COUNT);
    config_unlock();

    sstr_free(in);

    /* Handle password sentinel / hash */
    config_lock();
    const char *new_pw_str = sstr_cstr(g_config.web_password);
    if (strcmp(new_pw_str, "***UNCHANGED***") == 0 || strlen(new_pw_str) == 0) {
        sstr_free(g_config.web_password);
        g_config.web_password = prev_pw;
        prev_pw = NULL;
    } else {
        char hashed[80];
        bool ok = utils_crypto_hash_password(new_pw_str, hashed, sizeof(hashed));
        sstr_free(g_config.web_password);
        if (ok) {
            g_config.web_password = sstr(hashed);
            sstr_free(prev_pw);
        } else {
            ESP_LOGW(TAG, "Password hashing failed, keeping previous");
            g_config.web_password = prev_pw;
        }
        prev_pw = NULL;
    }
    config_unlock();

    config_mark_dirty();

    /* Apply hostname to mDNS and Ethernet netif */
    config_lock();
    char hostname[64];
    strlcpy(hostname, sstr_cstr(g_config.hostname), sizeof(hostname));
    config_unlock();

    mdns_hostname_set(hostname);
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (netif) esp_netif_set_hostname(netif, hostname);
    ESP_LOGI(TAG, "Hostname updated to: %s", hostname);

    /* Apply MQTT config */
    mqtt_apply_config();
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    char *buf = malloc((size_t)req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) { free(buf); return received == 0 ? ESP_OK : ESP_FAIL; }
    buf[received] = '\0';

    apply_settings_from_buf(buf, received);
    free(buf);

    return send_settings_json(req);
}

/* ── Backup ──────────────────────────────────────────────────────────────── */

static esp_err_t backup_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    config_lock();
    sstr_t json = sstr_new();
    json_marshal_gateway_config_t(&g_config, json);
    config_unlock();

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

/* ── Restore ─────────────────────────────────────────────────────────────── */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

static esp_err_t restore_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->content_len <= 0 || req->content_len > 32768) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body");
        return ESP_OK;
    }

    FILE *f = fopen("/littlefs/config.json", "w");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_OK;
    }

    char chunk[256];
    int remaining = req->content_len;
    bool write_ok = true;
    while (remaining > 0) {
        int to_read = remaining < (int)sizeof(chunk) ? remaining : (int)sizeof(chunk);
        int got = httpd_req_recv(req, chunk, to_read);
        if (got <= 0) { write_ok = false; break; }
        if (fwrite(chunk, 1, (size_t)got, f) != (size_t)got) { write_ok = false; break; }
        remaining -= got;
    }
    fclose(f);

    if (!write_ok) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── /api/info ───────────────────────────────────────────────────────────── */

static const char *language_to_str(int lang)
{
    switch (lang) {
    case language_t_pl: return "pl";
    default:            return "en";
    }
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    struct web_info_t info;
    web_info_t_init(&info);

    char pw[128] = {0};
    bool has_header =
        httpd_req_get_hdr_value_str(req, "X-Auth", pw, sizeof(pw)) == ESP_OK;

    config_lock();
    info.web_password_enabled = g_config.web_password_enabled;
    info.language = sstr(language_to_str(g_config.language));
    if (has_header) {
        info.has_web_password_valid = 1;
        info.web_password_valid =
            utils_crypto_verify_password(pw, sstr_cstr(g_config.web_password));
    }
    config_unlock();

    sstr_t json = sstr_new();
    json_marshal_web_info_t(&info, json);
    web_info_t_clear(&info);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

/* ── File API helpers ────────────────────────────────────────────────────── */

static void url_decode(char *str)
{
    char *r = str, *w = str;
    while (*r) {
        if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
            char hex[3] = { r[1], r[2], '\0' };
            *w++ = (char)strtol(hex, NULL, 16);
            r += 3;
        } else if (*r == '+') {
            *w++ = ' ';
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
}

static bool sanitize_sdcard_path(const char *input, char *out, size_t out_size)
{
    /* Reject any path component equal to ".." */
    const char *p = input;
    while (*p) {
        if (p[0] == '.' && p[1] == '.' && (p[2] == '/' || p[2] == '\0'))
            return false;
        while (*p && *p != '/') p++;
        if (*p == '/') p++;
    }

    if (strncmp(input, SDCARD_MOUNT_POINT, strlen(SDCARD_MOUNT_POINT)) == 0) {
        snprintf(out, out_size, "%s", input);
    } else {
        /* Strip leading slash from input before prepending mount point */
        const char *rel = input;
        while (*rel == '/') rel++;
        if (*rel)
            snprintf(out, out_size, "%s/%s", SDCARD_MOUNT_POINT, rel);
        else
            snprintf(out, out_size, "%s", SDCARD_MOUNT_POINT);
    }
    return true;
}

/* Append a JSON-escaped string to an sstr_t */
static void sstr_append_json_str(sstr_t s, const char *str)
{
    sstr_append_cstr(s, "\"");
    for (const char *c = str; *c; c++) {
        if (*c == '"')       sstr_append_cstr(s, "\\\"");
        else if (*c == '\\') sstr_append_cstr(s, "\\\\");
        else if (*c == '\n') sstr_append_cstr(s, "\\n");
        else if (*c == '\r') sstr_append_cstr(s, "\\r");
        else if (*c == '\t') sstr_append_cstr(s, "\\t");
        else                 sstr_append_of(s, c, 1);
    }
    sstr_append_cstr(s, "\"");
}

static esp_err_t files_list_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char query[256] = {0};
    char path_param[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "path", path_param, sizeof(path_param));
    url_decode(path_param);

    char full_path[256];
    if (!sanitize_sdcard_path(path_param[0] ? path_param : "/", full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }

    if (!sdcard_is_mounted()) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "{\"error\":\"not_mounted\"}");
        return ESP_OK;
    }

    DIR *dir = opendir(full_path);
    if (!dir) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    sstr_t json = sstr_new();
    sstr_append_cstr(json, "[");
    bool first = true;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        char entry_path[512];
        snprintf(entry_path, sizeof(entry_path), "%s/%s", full_path, ent->d_name);
        struct stat st = {0};
        stat(entry_path, &st);

        if (!first) sstr_append_cstr(json, ",");
        first = false;
        sstr_append_cstr(json, "{\"name\":");
        sstr_append_json_str(json, ent->d_name);
        char num[32];
        snprintf(num, sizeof(num), ",\"size\":%ld", (long)st.st_size);
        sstr_append_cstr(json, num);
        sstr_append_cstr(json, ent->d_type == DT_DIR ? ",\"is_dir\":true}" : ",\"is_dir\":false}");
    }
    closedir(dir);
    sstr_append_cstr(json, "]");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, sstr_cstr(json), (ssize_t)sstr_length(json));
    sstr_free(json);
    return ESP_OK;
}

static esp_err_t files_download_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    char query[256] = {0};
    char path_param[128] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "path", path_param, sizeof(path_param));
    url_decode(path_param);

    if (!path_param[0]) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "missing path");
        return ESP_OK;
    }

    char full_path[256];
    if (!sanitize_sdcard_path(path_param, full_path, sizeof(full_path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid path");
        return ESP_OK;
    }

    if (!sdcard_is_mounted()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "SD card not available");
        return ESP_OK;
    }

    FILE *f = fopen(full_path, "r");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
        return ESP_OK;
    }

    /* Content-Length (best-effort; allows client to show progress) */
    char content_len_buf[24] = {0};
    struct stat st;
    if (stat(full_path, &st) == 0)
        snprintf(content_len_buf, sizeof(content_len_buf), "%lld", (long long)st.st_size);
    if (content_len_buf[0])
        httpd_resp_set_hdr(req, "Content-Length", content_len_buf);

    /* Extract basename for Content-Disposition */
    const char *basename = strrchr(full_path, '/');
    basename = basename ? basename + 1 : full_path;

    size_t disp_len = strlen("attachment; filename=\"\"") + strlen(basename) + 1;
    char *disp = malloc(disp_len);
    if (!disp) { fclose(f); return ESP_ERR_NO_MEM; }
    snprintf(disp, disp_len, "attachment; filename=\"%s\"", basename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");

    char *buf = malloc(4096);
    if (!buf) { free(disp); fclose(f); return ESP_ERR_NO_MEM; }

    size_t n;
    while ((n = fread(buf, 1, 4096, f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, (ssize_t)n) != ESP_OK) {
            free(buf);
            free(disp);
            fclose(f);
            return ESP_OK;
        }
    }
    free(buf);
    free(disp);
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

#define MAX_DOOR_OPEN_MS (5 * 60 * 1000)

/* ── /api/open ───────────────────────────────────────────────────────────── */

static esp_err_t open_post_handler(httpd_req_t *req)
{
    char query[512] = {0};
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char pw_param[128] = {0};
    httpd_query_key_value(query, "password", pw_param, sizeof(pw_param));

    char reason[128] = {0};
    httpd_query_key_value(query, "reason", reason, sizeof(reason));
    url_decode(reason);

    char username[128] = {0};
    httpd_query_key_value(query, "username", username, sizeof(username));
    url_decode(username);

    char open_time_str[32] = {0};
    httpd_query_key_value(query, "open_time_ms", open_time_str, sizeof(open_time_str));

    config_lock();
    bool enabled = sstr_length(g_config.remote_open_password) > 0;
    bool ok = enabled &&
              strcmp(pw_param, sstr_cstr(g_config.remote_open_password)) == 0;
    int relay_open_ms = g_config.relay_open_ms;
    config_unlock();

    if (!enabled) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "remote open not configured");
        return ESP_OK;
    }
    if (!ok) {
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "wrong password");
        return ESP_OK;
    }

    if (open_time_str[0]) {
        int override_ms = atoi(open_time_str);
        if (override_ms >= 100 && override_ms <= MAX_DOOR_OPEN_MS)
            relay_open_ms = override_ms;
    }

    ESP_LOGI(TAG, "Remote open: reason='%s' username='%s' open_time_ms=%d",
             reason[0] ? reason : "(none)", username[0] ? username : "(none)",
             relay_open_ms);

    outputs_play_pattern(OUTPUT_RELAY,      relay_open_ms, 0, 1);
    outputs_play_pattern(OUTPUT_LED_READER, relay_open_ms, 0, 1);

    event_emit_remote_open(reason, username, relay_open_ms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

/* ── /api/control ────────────────────────────────────────────────────────── */

/* Manual output triggers for the admin UI. Unlike /api/open these sit behind
 * the web admin password, not remote_open_password. */

/* Reads the optional duration_ms query param into *out (def when absent).
 * Sends 400 and returns false when it is present but out of range. */
static bool parse_duration_query(httpd_req_t *req, int def, int min, int max, int *out)
{
    *out = def;

    char query[128] = {0};
    char val[16]    = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "duration_ms", val, sizeof(val)) != ESP_OK ||
        val[0] == '\0')
        return true;

    char *end;
    long ms = strtol(val, &end, 10);
    if (*end != '\0' || ms < min || ms > max) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "duration_ms out of range");
        return false;
    }
    *out = (int)ms;
    return true;
}

static esp_err_t control_open_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    config_lock();
    int relay_open_ms = g_config.relay_open_ms;
    config_unlock();

    int ms;
    if (!parse_duration_query(req, relay_open_ms, 100, MAX_DOOR_OPEN_MS, &ms)) return ESP_OK;

    ESP_LOGI(TAG, "Control: open lock for %d ms", ms);
    outputs_play_pattern(OUTPUT_RELAY,      ms, 0, 1);
    outputs_play_pattern(OUTPUT_LED_READER, ms, 0, 1);
    event_emit_remote_open("admin UI", NULL, ms);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t control_beep_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    int ms;
    if (!parse_duration_query(req, 200, 10, 5000, &ms)) return ESP_OK;

    ESP_LOGI(TAG, "Control: beep for %d ms", ms);
    outputs_play_pattern(OUTPUT_BEEPER, ms, 0, 1);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t control_led_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    int ms;
    if (!parse_duration_query(req, 3000, 10, 60000, &ms)) return ESP_OK;

    ESP_LOGI(TAG, "Control: reader LED on for %d ms", ms);
    outputs_play_pattern(OUTPUT_LED_READER, ms, 0, 1);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t control_reboot_post_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    ESP_LOGI(TAG, "Control: reboot requested");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    xTaskCreate(reboot_task, "reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── Early init ──────────────────────────────────────────────────────────── */

void webserver_early_init(void)
{
    s_ws_mutex = xSemaphoreCreateMutex();
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        s_ws_fds[i] = -1;
}

/* ── webserver_start ─────────────────────────────────────────────────────── */

void webserver_start(void)
{
    if (s_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 24;
    config.stack_size       = 8192;

    ESP_LOGI(TAG, "Starting HTTP server on port %d", config.server_port);
    if (httpd_start(&s_server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return;
    }

    static const httpd_uri_t uri_root = {
        .uri = "/", .method = HTTP_GET, .handler = index_html_handler,
    };
    static const httpd_uri_t uri_js = {
        .uri = "/app.js", .method = HTTP_GET, .handler = app_js_handler,
    };
    static const httpd_uri_t uri_css = {
        .uri = "/app.css", .method = HTTP_GET, .handler = app_css_handler,
    };
    static const httpd_uri_t uri_status_get = {
        .uri     = "/api/status",
        .method  = HTTP_GET,
        .handler = status_get_handler,
    };
    static const httpd_uri_t uri_settings_get = {
        .uri     = "/api/settings",
        .method  = HTTP_GET,
        .handler = settings_get_handler,
    };
    static const httpd_uri_t uri_settings_post = {
        .uri     = "/api/settings",
        .method  = HTTP_POST,
        .handler = settings_post_handler,
    };
    static const httpd_uri_t uri_restore_post = {
        .uri     = "/api/restore",
        .method  = HTTP_POST,
        .handler = restore_post_handler,
    };
    static const httpd_uri_t uri_backup_get = {
        .uri     = "/api/backup",
        .method  = HTTP_GET,
        .handler = backup_get_handler,
    };
    static const httpd_uri_t uri_info_get = {
        .uri     = "/api/info",
        .method  = HTTP_GET,
        .handler = info_get_handler,
    };
    static const httpd_uri_t uri_files_list = {
        .uri     = "/api/files",
        .method  = HTTP_GET,
        .handler = files_list_handler,
    };
    static const httpd_uri_t uri_files_download = {
        .uri     = "/api/files/download",
        .method  = HTTP_GET,
        .handler = files_download_handler,
    };
    static const httpd_uri_t uri_open_post = {
        .uri     = "/api/open",
        .method  = HTTP_POST,
        .handler = open_post_handler,
    };
    static const httpd_uri_t uri_control_open = {
        .uri     = "/api/control/open",
        .method  = HTTP_POST,
        .handler = control_open_post_handler,
    };
    static const httpd_uri_t uri_control_beep = {
        .uri     = "/api/control/beep",
        .method  = HTTP_POST,
        .handler = control_beep_post_handler,
    };
    static const httpd_uri_t uri_control_led = {
        .uri     = "/api/control/led",
        .method  = HTTP_POST,
        .handler = control_led_post_handler,
    };
    static const httpd_uri_t uri_control_reboot = {
        .uri     = "/api/control/reboot",
        .method  = HTTP_POST,
        .handler = control_reboot_post_handler,
    };
    static const httpd_uri_t uri_events_get = {
        .uri     = "/api/events",
        .method  = HTTP_GET,
        .handler = events_get_handler,
    };
    static const httpd_uri_t uri_ws = {
        .uri                  = "/ws",
        .method               = HTTP_GET,
        .handler              = ws_handler,
        .is_websocket         = true,
        .ws_pre_handshake_cb  = ws_pre_handshake_cb,
        .ws_post_handshake_cb = ws_post_handshake_cb,
    };

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_status_get);
    httpd_register_uri_handler(s_server, &uri_settings_get);
    httpd_register_uri_handler(s_server, &uri_settings_post);
    httpd_register_uri_handler(s_server, &uri_restore_post);
    httpd_register_uri_handler(s_server, &uri_backup_get);
    httpd_register_uri_handler(s_server, &uri_info_get);
    httpd_register_uri_handler(s_server, &uri_files_list);
    httpd_register_uri_handler(s_server, &uri_files_download);
    httpd_register_uri_handler(s_server, &uri_open_post);
    httpd_register_uri_handler(s_server, &uri_control_open);
    httpd_register_uri_handler(s_server, &uri_control_beep);
    httpd_register_uri_handler(s_server, &uri_control_led);
    httpd_register_uri_handler(s_server, &uri_control_reboot);
    httpd_register_uri_handler(s_server, &uri_events_get);
    httpd_register_uri_handler(s_server, &uri_ws);

    ota_register_handlers(s_server);

    /* Heartbeat so uptime/clock keep ticking in the UI even when nothing
     * else changes. Event-driven pushes carry the latency-sensitive state. */
    esp_timer_handle_t status_timer;
    const esp_timer_create_args_t timer_args = {
        .callback = status_timer_cb,
        .name     = "ws_status",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &status_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(status_timer, STATUS_HEARTBEAT_US));

    ESP_LOGI(TAG, "HTTP server started");
}
