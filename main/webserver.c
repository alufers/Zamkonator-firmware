#include "webserver.h"
#include "background_worker.h"
#include "config.h"
#include "ethernet_manager.h"
#include "mqtt.h"
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
#include "mdns.h"

static const char *TAG = "webserver";

extern int g_mqtt_status; /* enum mqtt_status_t */

/* ── State ───────────────────────────────────────────────────────────────── */

static httpd_handle_t s_server = NULL;

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

/* ── /api/status ─────────────────────────────────────────────────────────── */

static esp_err_t status_get_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    struct ws_status_payload_t p;
    ws_status_payload_t_init(&p);
    p.uptime          = esp_timer_get_time() / 1000000LL;
    p.time            = (int64_t)time(NULL);
    p.mqtt_status     = g_mqtt_status;
    p.sd_card_mounted = sdcard_is_mounted();

    sstr_t json = sstr_new();
    json_marshal_indent_ws_status_payload_t(&p, 0, 0, json);
    ws_status_payload_t_clear(&p);

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

/* ── Early init ──────────────────────────────────────────────────────────── */

void webserver_early_init(void)
{
}

/* ── webserver_start ─────────────────────────────────────────────────────── */

void webserver_start(void)
{
    if (s_server) return;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    config.max_uri_handlers = 12;
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

    ESP_LOGI(TAG, "HTTP server started");
}
