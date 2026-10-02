#include "auth_proxy.h"
#include "auth.h"
#include "config.h"
#include "outputs.h"
#include "webserver.h"

#include <string.h>

#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "auth_proxy";

static volatile bool s_healthy = false;
static TaskHandle_t  s_task    = NULL;

bool auth_proxy_is_healthy(void)
{
    return s_healthy;
}

/* Strip trailing slashes from src into out (NUL-terminated, max out_size bytes). */
static void strip_trailing_slash(const char *src, char *out, size_t out_size)
{
    size_t len = strlcpy(out, src, out_size);
    if (len >= out_size) len = out_size - 1;
    while (len > 0 && out[len - 1] == '/') out[--len] = '\0';
}

static void auth_proxy_monitor_task(void *arg)
{
    (void)arg;

    while (1) {
        char base_url[256] = {0};
        int  timeout_ms    = 10000;
        int  interval_ms   = 30000;

        config_lock();
        strip_trailing_slash(sstr_cstr(g_config.auth_proxy_base_url),
                             base_url, sizeof(base_url));
        timeout_ms  = g_config.auth_proxy_timeout_ms;
        interval_ms = g_config.auth_proxy_healthcheck_interval_ms;
        config_unlock();

        if (base_url[0] == '\0') {
            /* No proxy configured — treat as healthy so RED LED stays off */
            if (!s_healthy) {
                s_healthy = true;
                outputs_update_base_state();
            }
            xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(interval_ms));
            continue;
        }

        /* Blink red + reader LED to indicate a health check is starting */
        outputs_flash_invert(OUTPUT_LED_RED,    100);
        outputs_flash_invert(OUTPUT_LED_READER, 100);

        char url[320];
        snprintf(url, sizeof(url), "%s/users/-/stats", base_url);

        esp_http_client_config_t http_cfg = {
            .url        = url,
            .timeout_ms = timeout_ms,
            .method     = HTTP_METHOD_GET,
        };
        esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

        bool ok = false;
        esp_err_t err = esp_http_client_open(client, 0);
        if (err == ESP_OK) {
            esp_http_client_fetch_headers(client);
            ok = (esp_http_client_get_status_code(client) == 200);
        } else {
            ESP_LOGW(TAG, "Stats request failed: %s", esp_err_to_name(err));
        }
        esp_http_client_close(client);
        esp_http_client_cleanup(client);

        bool prev = s_healthy;
        s_healthy = ok;
        if (prev != ok) {
            outputs_update_base_state();
            webserver_push_status();
            /* Coming back online is the best moment to refresh the offline
             * cache — the proxy is known reachable right now. */
            if (ok) auth_cache_trigger_download();
        }

        if (ok) {
            ESP_LOGI(TAG, "Auth proxy healthy");
        } else {
            ESP_LOGW(TAG, "Auth proxy unhealthy");
            outputs_play_pattern(OUTPUT_LED_READER, 100, 100, 5);
        }

        xTaskNotifyWait(0, 0, NULL, pdMS_TO_TICKS(interval_ms));
    }
}

void auth_proxy_trigger_check(void)
{
    if (s_task)
        xTaskNotify(s_task, 0, eNoAction);
}

void auth_proxy_monitor_init(void)
{
    xTaskCreate(auth_proxy_monitor_task, "auth_proxy_mon", 8192, NULL, 4, &s_task);
    ESP_LOGI(TAG, "Auth proxy monitor started");
}
