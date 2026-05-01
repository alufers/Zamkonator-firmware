#include "ota.h"
#include "config.h"
#include "utils.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"

static const char *TAG = "ota";

/* ── Auth helper ─────────────────────────────────────────────────────────── */

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

#define REQUIRE_AUTH(req)                                                      \
    do {                                                                       \
        if (!check_auth(req)) {                                                \
            httpd_resp_set_hdr(req, "WWW-Authenticate", "X-Auth");             \
            httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");  \
            return ESP_OK;                                                     \
        }                                                                      \
    } while (0)

/* ── Upload state ────────────────────────────────────────────────────────── */

typedef enum {
    OTA_UPLOAD_IDLE,
    OTA_UPLOAD_IN_PROGRESS,
    OTA_UPLOAD_SUCCESS,
    OTA_UPLOAD_ERROR,
} ota_upload_state_t;

static struct {
    ota_upload_state_t state;
    size_t bytes_written;
    size_t bytes_total;
    char error[128];
} s_ota;

static SemaphoreHandle_t s_ota_mutex;

/* ── Rollback verification ───────────────────────────────────────────────── */

static esp_timer_handle_t s_verify_timer;

static void verify_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Verification timer fired — marking app valid, cancelling rollback");
    esp_ota_mark_app_valid_cancel_rollback();
}

static void ota_got_ip_handler(void *arg, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    ESP_LOGI(TAG, "Got IP — starting 20 s OTA verification timer");
    esp_timer_start_once(s_verify_timer, 20000000ULL); /* 20 s in microseconds */
    esp_event_handler_unregister(IP_EVENT, IP_EVENT_ETH_GOT_IP, ota_got_ip_handler);
}

void ota_init(void)
{
    s_ota_mutex = xSemaphoreCreateMutex();

    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    ESP_LOGI(TAG, "Booted into OTA image pending verify — will approve after IP + 20 s");

    esp_timer_create_args_t ta = {
        .callback = verify_timer_cb,
        .name     = "ota_verify",
    };
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_verify_timer));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               ota_got_ip_handler, NULL));
}

/* ── Reboot task ─────────────────────────────────────────────────────────── */

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(400));
    esp_restart();
}

/* ── /api/ota/upload ─────────────────────────────────────────────────────── */

#define OTA_BUF_SIZE 4096
#define OTA_MAX_SIZE (2 * 1024 * 1024)

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (req->content_len == 0 || req->content_len > OTA_MAX_SIZE) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid content length");
        return ESP_OK;
    }

    if (!s_ota_mutex) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "not initialized");
        return ESP_OK;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    if (s_ota.state == OTA_UPLOAD_IN_PROGRESS) {
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"error\":\"update already in progress\"}");
        return ESP_OK;
    }
    s_ota.state        = OTA_UPLOAD_IN_PROGRESS;
    s_ota.bytes_written = 0;
    s_ota.bytes_total  = (size_t)req->content_len;
    s_ota.error[0]     = '\0';
    xSemaphoreGive(s_ota_mutex);

    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.state = OTA_UPLOAD_ERROR;
        strlcpy(s_ota.error, "no OTA partition available", sizeof(s_ota.error));
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
        return ESP_OK;
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.state = OTA_UPLOAD_ERROR;
        snprintf(s_ota.error, sizeof(s_ota.error), "esp_ota_begin: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "ota begin failed");
        return ESP_OK;
    }

    char *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.state = OTA_UPLOAD_ERROR;
        strlcpy(s_ota.error, "OOM", sizeof(s_ota.error));
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OOM");
        return ESP_OK;
    }

    int remaining = (int)req->content_len;
    while (remaining > 0) {
        int to_read = remaining < OTA_BUF_SIZE ? remaining : OTA_BUF_SIZE;
        int received = httpd_req_recv(req, buf, to_read);
        if (received <= 0) {
            free(buf);
            esp_ota_abort(handle);
            xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
            s_ota.state = OTA_UPLOAD_ERROR;
            strlcpy(s_ota.error, "recv failed", sizeof(s_ota.error));
            xSemaphoreGive(s_ota_mutex);
            return received == 0 ? ESP_OK : ESP_FAIL;
        }

        err = esp_ota_write(handle, buf, (size_t)received);
        if (err != ESP_OK) {
            free(buf);
            esp_ota_abort(handle);
            xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
            s_ota.state = OTA_UPLOAD_ERROR;
            snprintf(s_ota.error, sizeof(s_ota.error), "esp_ota_write: %s",
                     esp_err_to_name(err));
            xSemaphoreGive(s_ota_mutex);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "write failed");
            return ESP_OK;
        }

        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.bytes_written += (size_t)received;
        xSemaphoreGive(s_ota_mutex);
        remaining -= received;
    }

    free(buf);

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.state = OTA_UPLOAD_ERROR;
        snprintf(s_ota.error, sizeof(s_ota.error), "esp_ota_end: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            err == ESP_ERR_OTA_VALIDATE_FAILED
                                ? "image validation failed"
                                : "ota end failed");
        return ESP_OK;
    }

    err = esp_ota_set_boot_partition(partition);
    if (err != ESP_OK) {
        xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
        s_ota.state = OTA_UPLOAD_ERROR;
        snprintf(s_ota.error, sizeof(s_ota.error), "esp_ota_set_boot_partition: %s",
                 esp_err_to_name(err));
        xSemaphoreGive(s_ota_mutex);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                            "set boot partition failed");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "OTA image written, rebooting into new firmware");

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    s_ota.state = OTA_UPLOAD_SUCCESS;
    xSemaphoreGive(s_ota_mutex);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");

    xTaskCreate(reboot_task, "ota_reboot", 1024, NULL, 5, NULL);
    return ESP_OK;
}

/* ── /api/ota/status ─────────────────────────────────────────────────────── */

static esp_err_t ota_status_handler(httpd_req_t *req)
{
    REQUIRE_AUTH(req);

    if (!s_ota_mutex) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"state\":\"idle\",\"bytes_written\":0,\"bytes_total\":0}");
        return ESP_OK;
    }

    xSemaphoreTake(s_ota_mutex, portMAX_DELAY);
    ota_upload_state_t state  = s_ota.state;
    size_t written            = s_ota.bytes_written;
    size_t total              = s_ota.bytes_total;
    char error_copy[128];
    strlcpy(error_copy, s_ota.error, sizeof(error_copy));
    xSemaphoreGive(s_ota_mutex);

    const char *state_str;
    switch (state) {
    case OTA_UPLOAD_IDLE:        state_str = "idle";       break;
    case OTA_UPLOAD_IN_PROGRESS: state_str = "uploading";  break;
    case OTA_UPLOAD_SUCCESS:     state_str = "success";    break;
    case OTA_UPLOAD_ERROR:       state_str = "error";      break;
    default:                     state_str = "unknown";    break;
    }

    char json[320];
    if (state == OTA_UPLOAD_ERROR) {
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"error\":\"%s\"}",
                 state_str, error_copy);
    } else {
        snprintf(json, sizeof(json),
                 "{\"state\":\"%s\",\"bytes_written\":%zu,\"bytes_total\":%zu}",
                 state_str, written, total);
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* ── Handler registration ────────────────────────────────────────────────── */

void ota_register_handlers(httpd_handle_t server)
{
    static const httpd_uri_t uri_upload = {
        .uri     = "/api/ota/upload",
        .method  = HTTP_POST,
        .handler = ota_upload_handler,
    };
    static const httpd_uri_t uri_status = {
        .uri     = "/api/ota/status",
        .method  = HTTP_GET,
        .handler = ota_status_handler,
    };
    httpd_register_uri_handler(server, &uri_upload);
    httpd_register_uri_handler(server, &uri_status);
}
