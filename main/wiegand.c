#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <string.h>
#include <time.h>

#include "driver/gpio.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "auth_proxy.h"
#include "config.h"
#include "hw.h"
#include "outputs.h"
#include "utils.h"
#include "wiegand.h"

/* json-gen-c generated header (present after build) */
#include "json.gen.h"

#define WIEGAND_MAX_BITS       64
#define WIEGAND_TIMEOUT_MS     25
/* Reject pulses within 500 µs of the previous one — filters optocoupler ringing.
 * Wiegand bit period is ~2 ms, so this is well within the valid range. */
#define WIEGAND_DEBOUNCE_US    500

#define AUTH_RESPONSE_BUF_SIZE 4096

static const char *TAG = "wiegand";
static QueueHandle_t       s_bit_queue;
static volatile int64_t    s_last_bit_us;

static void IRAM_ATTR dat0_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_bit_us) < WIEGAND_DEBOUNCE_US) return;
    s_last_bit_us = now;
    uint8_t bit = 0;
    xQueueSendFromISR(s_bit_queue, &bit, NULL);
}

static void IRAM_ATTR dat1_isr(void *arg)
{
    int64_t now = esp_timer_get_time();
    if ((now - s_last_bit_us) < WIEGAND_DEBOUNCE_US) return;
    s_last_bit_us = now;
    uint8_t bit = 1;
    xQueueSendFromISR(s_bit_queue, &bit, NULL);
}

/* ── Outcome patterns ───────────────────────────────────────────────────── */

static void pattern_comm_error(void)
{
    outputs_play_pattern(OUTPUT_BEEPER,     100, 100, 5);
    outputs_play_pattern(OUTPUT_LED_RED,    100, 100, 5);
    outputs_play_pattern(OUTPUT_LED_READER, 100, 100, 5);
}

static void pattern_not_found(void)
{
    outputs_play_pattern(OUTPUT_BEEPER,     800, 200, 5);
    outputs_play_pattern(OUTPUT_LED_RED,    800, 200, 5);
    outputs_play_pattern(OUTPUT_LED_READER, 800, 200, 5);
}

static void pattern_expired(void)
{
    outputs_play_pattern(OUTPUT_BEEPER,     800, 200, 3);
    outputs_play_pattern(OUTPUT_LED_RED,    800, 200, 3);
    outputs_play_pattern(OUTPUT_LED_READER, 800, 200, 3);
}

static void pattern_access_granted(int relay_open_ms)
{
    outputs_play_pattern(OUTPUT_RELAY,      relay_open_ms, 0, 1);
    outputs_play_pattern(OUTPUT_BEEPER,     100,           0, 1);
    outputs_play_pattern(OUTPUT_LED_READER, relay_open_ms, 0, 1);
}

/* ── Auth proxy request ─────────────────────────────────────────────────── */

static void process_card(uint32_t mifare_id)
{
    /* Snapshot config under lock */
    char base_url[256] = {0};
    int  timeout_ms    = 0;
    int  relay_open_ms = 0;

    config_lock();
    size_t blen = strlcpy(base_url, sstr_cstr(g_config.auth_proxy_base_url),
                          sizeof(base_url));
    if (blen >= sizeof(base_url)) blen = sizeof(base_url) - 1;
    timeout_ms    = g_config.auth_proxy_timeout_ms;
    relay_open_ms = g_config.relay_open_ms;
    config_unlock();

    /* Strip trailing slash */
    while (blen > 0 && base_url[blen - 1] == '/') base_url[--blen] = '\0';

    if (blen == 0) {
        ESP_LOGW(TAG, "auth_proxy_base_url not configured, skipping auth");
        return;
    }

    if (!auth_proxy_is_healthy()) {
        ESP_LOGW(TAG, "Auth proxy unhealthy, denying access");
        pattern_comm_error();
        return;
    }

    char card_id[9];
    snprintf(card_id, sizeof(card_id), "%08" PRIx32, mifare_id);

    char url[320];
    snprintf(url, sizeof(url), "%s/users/-/by-card/%s", base_url, card_id);

    ESP_LOGI(TAG, "Authenticating card %s via %s", card_id, url);

    /* Blink reader LED continuously while waiting for the response */
    outputs_play_pattern(OUTPUT_LED_READER, 100, 100, 1000);

    esp_http_client_config_t http_cfg = {
        .url        = url,
        .timeout_ms = timeout_ms,
        .method     = HTTP_METHOD_GET,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        pattern_comm_error();
        return;
    }

    int content_len = esp_http_client_fetch_headers(client);
    int status      = esp_http_client_get_status_code(client);

    ESP_LOGI(TAG, "Auth response: status=%d content_len=%d", status, content_len);

    if (status == 404) {
        ESP_LOGW(TAG, "Card %s not found in auth proxy", card_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        pattern_not_found();
        return;
    }

    if (status != 200) {
        ESP_LOGE(TAG, "Unexpected HTTP status %d for card %s", status, card_id);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        pattern_comm_error();
        return;
    }

    /* Read body — allocate heap, cap at AUTH_RESPONSE_BUF_SIZE */
    int to_read = (content_len > 0 && content_len < AUTH_RESPONSE_BUF_SIZE - 1)
                  ? content_len
                  : AUTH_RESPONSE_BUF_SIZE - 1;

    char *buf = malloc(AUTH_RESPONSE_BUF_SIZE);
    if (!buf) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        pattern_comm_error();
        return;
    }

    int nread = esp_http_client_read(client, buf, to_read);
    buf[nread > 0 ? nread : 0] = '\0';

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (nread <= 0) {
        ESP_LOGE(TAG, "Empty body for card %s", card_id);
        free(buf);
        pattern_comm_error();
        return;
    }

    /* Parse JSON response */
    struct auth_proxy_user_t user;
    auth_proxy_user_t_init(&user);
    sstr_t in = sstr_of(buf, (size_t)nread);
    int rc = json_unmarshal_auth_proxy_user_t(in, &user);
    sstr_free(in);
    free(buf);

    if (rc != 0) {
        ESP_LOGE(TAG, "JSON parse error (rc=%d) for card %s", rc, card_id);
        auth_proxy_user_t_clear(&user);
        pattern_comm_error();
        return;
    }

    /* Check membership expiration if system clock is valid */
    if (utils_time_is_valid()
        && user.membership_expiration > 0
        && (time_t)user.membership_expiration < time(NULL)) {
        ESP_LOGW(TAG, "Membership expired for %s (exp=%lld now=%lld)",
                 sstr_cstr(user.uid),
                 (long long)user.membership_expiration, (long long)time(NULL));
        auth_proxy_user_t_clear(&user);
        pattern_expired();
        return;
    }

    ESP_LOGI(TAG, "Access granted for %s (card %s)", sstr_cstr(user.uid), card_id);
    auth_proxy_user_t_clear(&user);

    pattern_access_granted(relay_open_ms);
}

/* ── Wiegand reader task ─────────────────────────────────────────────────── */

static void wiegand_task(void *arg)
{
    uint8_t  bit;
    uint64_t data      = 0;
    int      bit_count = 0;

    while (1) {
        if (xQueueReceive(s_bit_queue, &bit, pdMS_TO_TICKS(WIEGAND_TIMEOUT_MS)) == pdTRUE) {
            if (bit_count < WIEGAND_MAX_BITS) {
                data = (data << 1) | bit;
                bit_count++;
            }
        } else if (bit_count > 0) {
            /* Flash the green LED to acknowledge any card read attempt */
            outputs_flash_invert(OUTPUT_LED_GREEN, 100);

            if (bit_count == 34) {
                /* Wiegand 34: 1 even parity + 32 data bits + 1 odd parity.
                 * Data bits are MSB-first; MIFARE UID bytes are in reverse
                 * order, so byte-swap to recover the card UID. */
                uint32_t raw       = (uint32_t)((data >> 1) & 0xFFFFFFFF);
                uint32_t mifare_id = __builtin_bswap32(raw);
                ESP_LOGI(TAG, "Card scanned: mifare_id=%08" PRIx32, mifare_id);
                process_card(mifare_id);
            } else {
                ESP_LOGI(TAG, "Card scanned: %d bits, raw=0x%0*llX",
                         bit_count, (bit_count + 3) / 4, (unsigned long long)data);
            }
            data      = 0;
            bit_count = 0;
        }
    }
}

void wiegand_init(void)
{
    s_bit_queue = xQueueCreate(64, sizeof(uint8_t));

    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << HW_WIEGAND_DAT0_GPIO) |
                        (1ULL << HW_WIEGAND_DAT1_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_POSEDGE,
    };
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(HW_WIEGAND_DAT0_GPIO, dat0_isr, NULL);
    gpio_isr_handler_add(HW_WIEGAND_DAT1_GPIO, dat1_isr, NULL);

    xTaskCreate(wiegand_task, "wiegand", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Wiegand reader initialised on DAT0=GPIO%d DAT1=GPIO%d",
             HW_WIEGAND_DAT0_GPIO, HW_WIEGAND_DAT1_GPIO);
}
