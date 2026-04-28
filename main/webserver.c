#include "webserver.h"
#include "background_worker.h"
#include "config.h"
#include "ethernet_manager.h"
#include "mqtt.h"
#include "utils.h"

#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
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

#define MAX_WS_CLIENTS    8
#define CONSOLE_BUF_SIZE  512
#define HISTORY_BUF_SIZE  4096

/* ── State ───────────────────────────────────────────────────────────────── */

static httpd_handle_t    s_server   = NULL;
static int               s_ws_fds[MAX_WS_CLIENTS];
static SemaphoreHandle_t s_ws_mutex;

static char  s_history[HISTORY_BUF_SIZE];
static int   s_history_write = 0;
static bool  s_history_full  = false;

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

/* ── History buffer ──────────────────────────────────────────────────────── */

static void history_append_locked(const char *str, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_history[s_history_write] = str[i];
        s_history_write = (s_history_write + 1) % HISTORY_BUF_SIZE;
        if (s_history_write == 0)
            s_history_full = true;
    }
}

static char *history_snapshot(size_t *out_len)
{
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);

    int len  = s_history_full ? HISTORY_BUF_SIZE : s_history_write;
    int tail = s_history_write;

    if (len == 0) {
        xSemaphoreGive(s_ws_mutex);
        *out_len = 0;
        return NULL;
    }

    char *buf = malloc((size_t)len + 1);
    if (buf) {
        if (!s_history_full) {
            memcpy(buf, s_history, (size_t)len);
        } else {
            int first = HISTORY_BUF_SIZE - tail;
            memcpy(buf, s_history + tail, (size_t)first);
            memcpy(buf + first, s_history, (size_t)tail);
        }
        buf[len] = '\0';
    }

    xSemaphoreGive(s_ws_mutex);
    *out_len = (size_t)len;
    return buf;
}

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

/* ── WebSocket helpers ───────────────────────────────────────────────────── */

static void ws_remove_fd_locked(int fd)
{
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] == fd) s_ws_fds[i] = -1;
}

static void ws_send_history(int fd)
{
    size_t hist_len;
    char  *hist = history_snapshot(&hist_len);
    if (!hist) return;

    struct ws_server_message_t msg;
    ws_server_message_t_init(&msg);
    msg.tag = ws_server_message_t_console;
    msg.value.console.payload = sstr(hist);
    free(hist);

    sstr_t out = sstr_new();
    json_marshal_ws_server_message_t(&msg, out);
    ws_server_message_t_clear(&msg);
    ws_queue_send(fd, sstr_cstr(out));
    sstr_free(out);
}

/* ── WS broadcast / send helpers (public) ───────────────────────────────── */

void webserver_ws_broadcast_json(const char *json)
{
    if (!s_server) return;
    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], json);
}

void webserver_ws_send_json_to_fd(int fd, const char *json)
{
    ws_queue_send(fd, json);
}

/* ── Status broadcast ────────────────────────────────────────────────────── */

static void build_and_send_status(int fd /* -1 = broadcast */)
{
    time_t now     = time(NULL);
    int64_t uptime = esp_timer_get_time() / 1000000LL;

    struct ws_server_message_t msg;
    ws_server_message_t_init(&msg);
    msg.tag = ws_server_message_t_status;

    struct ws_status_payload_t *p = &msg.value.status.payload;
    p->uptime      = uptime;
    p->time        = (int64_t)now;
    p->mqtt_status = g_mqtt_status;

    sstr_t out = sstr_new();
    json_marshal_ws_server_message_t(&msg, out);
    ws_server_message_t_clear(&msg);

    if (fd == -1)
        webserver_ws_broadcast_json(sstr_cstr(out));
    else
        webserver_ws_send_json_to_fd(fd, sstr_cstr(out));
    sstr_free(out);
}

static void status_timer_cb(void *arg)
{
    (void)arg;
    build_and_send_status(-1);
}

void webserver_start_status_timer(void)
{
    esp_timer_handle_t t;
    esp_timer_create_args_t args = {
        .callback = status_timer_cb,
        .name     = "ws_status",
    };
    ESP_ERROR_CHECK(esp_timer_create(&args, &t));
    ESP_ERROR_CHECK(esp_timer_start_periodic(t, 10ULL * 1000 * 1000));
}

/* ── WebSocket handler ───────────────────────────────────────────────────── */

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* Auth check via ?auth=<password> query param */
        config_lock();
        bool pw_enabled = g_config.web_password_enabled;
        config_unlock();
        if (pw_enabled) {
            char query[256] = {0};
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
                return ESP_OK;
            }
        }

        int fd = httpd_req_to_sockfd(req);

        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        bool has_slot = false;
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_ws_fds[i] == -1) { has_slot = true; break; }
        }
        xSemaphoreGive(s_ws_mutex);

        if (!has_slot) {
            ESP_LOGW(TAG, "WS client list full, dropping fd=%d", fd);
            return ESP_OK;
        }

        ws_send_history(fd);
        build_and_send_status(fd);

        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_WS_CLIENTS; i++) {
            if (s_ws_fds[i] == -1) { s_ws_fds[i] = fd; break; }
        }
        xSemaphoreGive(s_ws_mutex);

        ESP_LOGI(TAG, "WS client connected fd=%d", fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;

    uint8_t *buf = NULL;
    if (frame.len > 0) {
        buf = calloc(1, frame.len + 1);
        if (!buf) return ESP_ERR_NO_MEM;
        frame.payload = buf;
        ret = httpd_ws_recv_frame(req, &frame, frame.len);
        if (ret != ESP_OK) { free(buf); return ret; }
    }

    if (frame.type == HTTPD_WS_TYPE_CLOSE) {
        int fd = httpd_req_to_sockfd(req);
        xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
        ws_remove_fd_locked(fd);
        xSemaphoreGive(s_ws_mutex);
        ESP_LOGI(TAG, "WS client disconnected fd=%d", fd);
    }

    free(buf);
    return ret;
}

/* ── gtw_console_log ─────────────────────────────────────────────────────── */

void gtw_console_log(const char *fmt, ...)
{
    char msg[CONSOLE_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    ESP_LOGI("gtw", "%s", msg);

    xSemaphoreTake(s_ws_mutex, portMAX_DELAY);
    history_append_locked(msg, strlen(msg));
    history_append_locked("\n", 1);
    int fds[MAX_WS_CLIENTS];
    int nfds = 0;
    for (int i = 0; i < MAX_WS_CLIENTS; i++)
        if (s_ws_fds[i] != -1) fds[nfds++] = s_ws_fds[i];
    xSemaphoreGive(s_ws_mutex);

    if (nfds == 0 || !s_server) return;

    struct ws_server_message_t ws_msg;
    ws_server_message_t_init(&ws_msg);
    ws_msg.tag = ws_server_message_t_console;
    ws_msg.value.console.payload = sstr(msg);

    sstr_t out = sstr_new();
    json_marshal_ws_server_message_t(&ws_msg, out);
    ws_server_message_t_clear(&ws_msg);

    for (int i = 0; i < nfds; i++)
        ws_queue_send(fds[i], sstr_cstr(out));
    sstr_free(out);
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
    config.max_uri_handlers = 10;
    config.stack_size       = 6500;

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
    static const httpd_uri_t uri_ws = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
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

    httpd_register_uri_handler(s_server, &uri_root);
    httpd_register_uri_handler(s_server, &uri_js);
    httpd_register_uri_handler(s_server, &uri_css);
    httpd_register_uri_handler(s_server, &uri_ws);
    httpd_register_uri_handler(s_server, &uri_settings_get);
    httpd_register_uri_handler(s_server, &uri_settings_post);
    httpd_register_uri_handler(s_server, &uri_restore_post);
    httpd_register_uri_handler(s_server, &uri_backup_get);
    httpd_register_uri_handler(s_server, &uri_info_get);

    ESP_LOGI(TAG, "HTTP server started");
}
