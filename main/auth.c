#include "auth.h"
#include "auth_proxy.h"
#include "config.h"
#include "sdcard.h"
#include "utils.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "json.gen.h"

static const char *TAG = "auth";

#define AUTH_RESPONSE_BUF_SIZE 4096

/* ── Cached user database ────────────────────────────────────────────────── */

/* On-disk record, little-endian, packed to exactly 37 bytes:
 *   0  1   flags        (reserved, currently always 0)
 *   1  4   card_id      uint32 — numeric value of the 8-hex-char MIFARE id
 *   5  8   expiration   uint64 — Unix timestamp of membership expiry
 *   13 24  username     NUL-padded, truncated
 * Records are sorted ascending by card_id so lookups can binary-search. */
#define REC_SIZE          37
#define REC_OFF_FLAGS     0
#define REC_OFF_CARD_ID   1
#define REC_OFF_EXPIRES   5
#define REC_OFF_USERNAME  13
#define REC_USERNAME_LEN  24

#define USERS_PATH     SDCARD_MOUNT_POINT "/zamkonator_users.dat"
#define USERS_TMP_PATH SDCARD_MOUNT_POINT "/zamkonator_users.tmp"
#define ETAG_PATH      SDCARD_MOUNT_POINT "/zamkonator_users.etag"
#define ETAG_MAX       96

static TaskHandle_t s_downloader_task = NULL;
static char         s_etag[ETAG_MAX]  = {0};

/* ── Online strategy ─────────────────────────────────────────────────────── */

static auth_result_t auth_online_check(uint32_t mifare_id, auth_user_t *out)
{
    char base_url[256] = {0};
    int  timeout_ms    = 10000;

    config_lock();
    size_t blen = strlcpy(base_url, sstr_cstr(g_config.auth_proxy_base_url),
                          sizeof(base_url));
    if (blen >= sizeof(base_url)) blen = sizeof(base_url) - 1;
    timeout_ms = g_config.auth_proxy_timeout_ms;
    config_unlock();

    while (blen > 0 && base_url[blen - 1] == '/') base_url[--blen] = '\0';

    if (blen == 0) {
        ESP_LOGW(TAG, "auth_proxy_base_url not configured");
        return AUTH_FAIL_UNCONFIGURED;
    }

    if (!auth_proxy_is_healthy()) {
        ESP_LOGW(TAG, "Auth proxy unhealthy");
        return AUTH_FAIL_RETRYABLE;
    }

    char card_id[9];
    snprintf(card_id, sizeof(card_id), "%08" PRIx32, mifare_id);

    char url[320];
    snprintf(url, sizeof(url), "%s/users/-/by-card/%s", base_url, card_id);

    ESP_LOGI(TAG, "Authenticating card %s via %s", card_id, url);

    esp_http_client_config_t http_cfg = {
        .url        = url,
        .timeout_ms = timeout_ms,
        .method     = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return AUTH_FAIL_RETRYABLE;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return AUTH_FAIL_RETRYABLE;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Auth response: status=%d content_len=%d", status, content_len);

    /* A 404 is a real verdict from a working proxy — do not fall back to the
     * cache, which would only be able to give a staler version of the same
     * answer. Everything else means we did not get a usable verdict. */
    if (status == 404) {
        ESP_LOGW(TAG, "Card %s not found in auth proxy", card_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return AUTH_DENIED_NOT_FOUND;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status %d for card %s", status, card_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return AUTH_FAIL_RETRYABLE;
    }

    int to_read = (content_len > 0 && content_len < AUTH_RESPONSE_BUF_SIZE - 1)
                  ? content_len
                  : AUTH_RESPONSE_BUF_SIZE - 1;

    char *buf = malloc(AUTH_RESPONSE_BUF_SIZE);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return AUTH_FAIL_RETRYABLE;
    }

    int nread = esp_http_client_read(client, buf, to_read);
    buf[nread > 0 ? nread : 0] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (nread <= 0) {
        ESP_LOGE(TAG, "Empty body for card %s", card_id);
        free(buf);
        return AUTH_FAIL_RETRYABLE;
    }

    struct auth_proxy_user_t user;
    auth_proxy_user_t_init(&user);
    sstr_t in = sstr_of(buf, (size_t)nread);
    int rc = json_unmarshal_auth_proxy_user_t(in, &user);
    sstr_free(in);
    free(buf);

    if (rc != 0) {
        ESP_LOGE(TAG, "JSON parse error (rc=%d) for card %s", rc, card_id);
        auth_proxy_user_t_clear(&user);
        return AUTH_FAIL_RETRYABLE;
    }

    strlcpy(out->username, sstr_cstr(user.uid), sizeof(out->username));
    out->membership_expiration = user.membership_expiration;
    auth_proxy_user_t_clear(&user);

    /* Expiry is only meaningful with a synced clock. */
    if (utils_time_is_valid()
        && out->membership_expiration > 0
        && (time_t)out->membership_expiration < time(NULL)) {
        ESP_LOGW(TAG, "Membership expired for %s (exp=%lld now=%lld)",
                 out->username, (long long)out->membership_expiration,
                 (long long)time(NULL));
        return AUTH_DENIED_EXPIRED;
    }

    ESP_LOGI(TAG, "Access granted for %s (card %s) [online]",
             out->username, card_id);
    return AUTH_GRANTED;
}

/* ── Cached strategy ─────────────────────────────────────────────────────── */

static uint32_t rec_card_id(const uint8_t *rec)
{
    uint32_t v;
    memcpy(&v, rec + REC_OFF_CARD_ID, sizeof(v));   /* file is little-endian, so is the ESP32 */
    return v;
}

static auth_result_t auth_cached_check(uint32_t mifare_id, auth_user_t *out)
{
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, cached lookup unavailable");
        return AUTH_FAIL_RETRYABLE;
    }

    struct stat st;
    if (stat(USERS_PATH, &st) != 0) {
        ESP_LOGW(TAG, "%s not present", USERS_PATH);
        return AUTH_FAIL_RETRYABLE;
    }
    if (st.st_size <= 0 || st.st_size % REC_SIZE != 0) {
        ESP_LOGE(TAG, "%s has invalid size %ld", USERS_PATH, (long)st.st_size);
        return AUTH_FAIL_RETRYABLE;
    }

    FILE *f = fopen(USERS_PATH, "rb");
    if (!f) return AUTH_FAIL_RETRYABLE;

    long    n     = (long)(st.st_size / REC_SIZE);
    long    lo    = 0;
    long    hi    = n - 1;
    bool    found = false;
    uint8_t rec[REC_SIZE];

    while (lo <= hi) {
        long mid = lo + (hi - lo) / 2;
        if (fseek(f, mid * REC_SIZE, SEEK_SET) != 0 ||
            fread(rec, 1, REC_SIZE, f) != REC_SIZE) {
            fclose(f);
            ESP_LOGE(TAG, "Read error in %s", USERS_PATH);
            return AUTH_FAIL_RETRYABLE;
        }

        uint32_t cid = rec_card_id(rec);
        if (cid == mifare_id)      { found = true; break; }
        else if (cid < mifare_id)  lo = mid + 1;
        else                       hi = mid - 1;
    }
    fclose(f);

    if (!found) {
        ESP_LOGW(TAG, "Card %08" PRIx32 " not in cached database", mifare_id);
        return AUTH_DENIED_NOT_FOUND;
    }

    memcpy(out->username, rec + REC_OFF_USERNAME, REC_USERNAME_LEN);
    out->username[REC_USERNAME_LEN] = '\0';

    uint64_t exp;
    memcpy(&exp, rec + REC_OFF_EXPIRES, sizeof(exp));
    out->membership_expiration = (int64_t)exp;

    if (utils_time_is_valid() && exp > 0 && (time_t)exp < time(NULL)) {
        ESP_LOGW(TAG, "Membership expired for %s [cached]", out->username);
        return AUTH_DENIED_EXPIRED;
    }

    ESP_LOGI(TAG, "Access granted for %s (card %08" PRIx32 ") [cached]",
             out->username, mifare_id);
    return AUTH_GRANTED;
}

/* ── Strategy dispatch ───────────────────────────────────────────────────── */

auth_result_t auth_check_card(uint32_t mifare_id, auth_user_t *out,
                              int *strategy_out)
{
    memset(out, 0, sizeof(*out));

    *strategy_out = auth_strategy_name_t_auth_proxy_online;
    auth_result_t res = auth_online_check(mifare_id, out);

    if (res != AUTH_FAIL_RETRYABLE)
        return res;

    config_lock();
    bool cache_enabled = g_config.auth_proxy_cache_enabled;
    config_unlock();

    if (!cache_enabled) {
        ESP_LOGW(TAG, "Online auth failed and cache is disabled");
        return res;
    }

    ESP_LOGI(TAG, "Online auth unavailable, falling back to cached database");
    *strategy_out = auth_strategy_name_t_auth_proxy_cached;
    return auth_cached_check(mifare_id, out);
}

/* ── ETag persistence ────────────────────────────────────────────────────── */

static bool s_etag_loaded = false;

/* Deferred until the card is actually mounted — auth_init() runs while the SD
 * hotplug task is still bringing the volume up. */
static void etag_load(void)
{
    if (s_etag_loaded) return;
    s_etag_loaded = true;

    FILE *f = fopen(ETAG_PATH, "r");
    if (!f) return;
    if (fgets(s_etag, sizeof(s_etag), f)) {
        size_t len = strlen(s_etag);
        while (len > 0 && (s_etag[len - 1] == '\n' || s_etag[len - 1] == '\r'))
            s_etag[--len] = '\0';
    }
    fclose(f);
    if (s_etag[0]) ESP_LOGI(TAG, "Loaded cached user DB ETag: %s", s_etag);
}

static void etag_store(const char *etag)
{
    strlcpy(s_etag, etag, sizeof(s_etag));
    FILE *f = fopen(ETAG_PATH, "w");
    if (!f) {
        ESP_LOGW(TAG, "Cannot persist ETag");
        return;
    }
    fputs(s_etag, f);
    fclose(f);
}

/* ── Downloader ──────────────────────────────────────────────────────────── */

static char s_resp_etag[ETAG_MAX];

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_HEADER &&
        evt->header_key && strcasecmp(evt->header_key, "ETag") == 0)
        strlcpy(s_resp_etag, evt->header_value ? evt->header_value : "",
                sizeof(s_resp_etag));
    return ESP_OK;
}

/* Fetch the user database, replacing the on-card copy only on a complete,
 * well-formed 200 response. Any other outcome leaves the existing file alone,
 * so a flaky proxy can never take the door offline. */
static void download_users_db(void)
{
    char base_url[256] = {0};
    int  timeout_ms    = 10000;

    config_lock();
    bool   enabled = g_config.auth_proxy_cache_enabled;
    size_t blen    = strlcpy(base_url, sstr_cstr(g_config.auth_proxy_base_url),
                             sizeof(base_url));
    if (blen >= sizeof(base_url)) blen = sizeof(base_url) - 1;
    timeout_ms = g_config.auth_proxy_timeout_ms;
    config_unlock();

    while (blen > 0 && base_url[blen - 1] == '/') base_url[--blen] = '\0';

    if (!enabled || blen == 0) return;
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, skipping user DB download");
        return;
    }

    etag_load();

    char url[320];
    snprintf(url, sizeof(url), "%s/users/-/export.bin", base_url);

    s_resp_etag[0] = '\0';

    esp_http_client_config_t http_cfg = {
        .url           = url,
        .timeout_ms    = timeout_ms,
        .method        = HTTP_METHOD_GET,
        .event_handler = http_event_cb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (!client) return;

    /* Conditional request — the SD card only gets rewritten when the user set
     * actually changed, which is the whole point of storing the ETag. */
    if (s_etag[0])
        esp_http_client_set_header(client, "If-None-Match", s_etag);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "User DB download failed to connect: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);

    if (status == 304) {
        ESP_LOGI(TAG, "Cached user DB unchanged (304)");
        goto cleanup;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "User DB download returned HTTP %d, keeping existing file",
                 status);
        goto cleanup;
    }

    FILE *tmp = fopen(USERS_TMP_PATH, "wb");
    if (!tmp) {
        ESP_LOGE(TAG, "Cannot open %s for writing", USERS_TMP_PATH);
        goto cleanup;
    }

    char *buf = malloc(1024);
    if (!buf) {
        fclose(tmp);
        remove(USERS_TMP_PATH);
        goto cleanup;
    }

    long total = 0;
    bool ok    = true;
    for (;;) {
        int nread = esp_http_client_read(client, buf, 1024);
        if (nread < 0) { ok = false; break; }
        if (nread == 0) break;
        if (fwrite(buf, 1, (size_t)nread, tmp) != (size_t)nread) {
            ok = false;
            break;
        }
        total += nread;
    }
    free(buf);
    fclose(tmp);

    /* A connection dropped mid-body still leaves a well-formed record count,
     * so the advertised length is the only reliable completeness check. */
    if (content_len > 0 && total != content_len) {
        ESP_LOGE(TAG, "Truncated user DB: got %ld of %d bytes", total, content_len);
        remove(USERS_TMP_PATH);
        goto cleanup;
    }
    if (!ok || total <= 0 || total % REC_SIZE != 0) {
        ESP_LOGE(TAG, "Discarding malformed user DB (%ld bytes, ok=%d)", total, ok);
        remove(USERS_TMP_PATH);
        goto cleanup;
    }

    /* FAT rename() does not replace an existing target. */
    remove(USERS_PATH);
    if (rename(USERS_TMP_PATH, USERS_PATH) != 0) {
        ESP_LOGE(TAG, "Cannot move %s into place", USERS_TMP_PATH);
        remove(USERS_TMP_PATH);
        goto cleanup;
    }

    ESP_LOGI(TAG, "Cached user DB updated: %ld records", total / REC_SIZE);
    if (s_resp_etag[0]) etag_store(s_resp_etag);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void downloader_task(void *arg)
{
    (void)arg;

    for (;;) {
        download_users_db();

        config_lock();
        int interval_ms = g_config.auth_proxy_cache_refresh_ms;
        config_unlock();
        if (interval_ms < 60000) interval_ms = 60000;

        xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(interval_ms));
    }
}

void auth_cache_trigger_download(void)
{
    if (s_downloader_task)
        xTaskNotify(s_downloader_task, 0, eNoAction);
}

void auth_init(void)
{
    xTaskCreate(downloader_task, "auth_cache", 8192, NULL, 4, &s_downloader_task);
    ESP_LOGI(TAG, "Auth cache downloader started");
}
