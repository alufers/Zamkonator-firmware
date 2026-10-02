#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "auth.h"
#include "config.h"
#include "event_manager.h"
#include "hw.h"
#include "mqtt.h"
#include "outputs.h"
#include "wiegand.h"

/* json-gen-c generated header (present after build) */
#include "json.gen.h"

#define WIEGAND_MAX_BITS       64
#define WIEGAND_TIMEOUT_MS     25
/* Reject pulses within 500 µs of the previous one — filters optocoupler ringing.
 * Wiegand bit period is ~2 ms, so this is well within the valid range. */
#define WIEGAND_DEBOUNCE_US    500

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

/* ── Card processing ────────────────────────────────────────────────────── */

/* Report a scan on every path: the event stream is the audit log, so a scan
 * that was rejected or never even reached a strategy still has to show up. */
static void report_scan(const char *card_id, int bit_count, int result,
                        const auth_user_t *user, const int *strategy)
{
    struct event_payload_t p;
    event_payload_t_init(&p);
    p.tag = event_payload_t_card_scanned;

    struct ev_card_scanned_t *ev = &p.value.card_scanned;
    ev->card_id   = sstr(card_id);
    ev->bit_count = bit_count;
    ev->result    = result;

    if (user && user->username[0]) {
        ev->has_username = 1;
        ev->username     = sstr(user->username);
        if (user->membership_expiration > 0) {
            ev->has_membership_expiration = 1;
            ev->membership_expiration     = user->membership_expiration;
        }
    }
    if (strategy) {
        ev->has_strategy = 1;
        ev->strategy     = *strategy;
    }

    /* Also mirror the scan onto a retained topic, so a dashboard can show the
     * last scan without replaying the event stream. */
    sstr_t json = sstr_new();
    json_marshal_ev_card_scanned_t(ev, json);
    mqtt_publish("card/last_scan", sstr_cstr(json), 0, true);
    sstr_free(json);

    /* Takes ownership of the sstr_t fields above. */
    event_manager_submit(&p);
}

static void process_card(uint32_t mifare_id, int bit_count)
{
    char card_id[9];
    snprintf(card_id, sizeof(card_id), "%08" PRIx32, mifare_id);

    int relay_open_ms;
    config_lock();
    relay_open_ms = g_config.relay_open_ms;
    config_unlock();

    /* Blink reader LED continuously while the lookup runs */
    outputs_play_pattern(OUTPUT_LED_READER, 100, 100, 1000);

    auth_user_t user;
    int         strategy = 0;
    auth_result_t res = auth_check_card(mifare_id, &user, &strategy);

    switch (res) {
    case AUTH_GRANTED:
        pattern_access_granted(relay_open_ms);
        report_scan(card_id, bit_count, card_scan_result_t_granted,
                    &user, &strategy);
        break;

    case AUTH_DENIED_NOT_FOUND:
        pattern_not_found();
        report_scan(card_id, bit_count, card_scan_result_t_denied_not_found,
                    NULL, &strategy);
        break;

    case AUTH_DENIED_EXPIRED:
        pattern_expired();
        report_scan(card_id, bit_count, card_scan_result_t_denied_expired,
                    &user, &strategy);
        break;

    case AUTH_FAIL_UNCONFIGURED:
        ESP_LOGW(TAG, "Auth not configured, ignoring card %s", card_id);
        report_scan(card_id, bit_count, card_scan_result_t_unconfigured,
                    NULL, NULL);
        break;

    case AUTH_FAIL_RETRYABLE:
    default:
        pattern_comm_error();
        report_scan(card_id, bit_count, card_scan_result_t_error_comm,
                    NULL, NULL);
        break;
    }
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
                process_card(mifare_id, bit_count);
            } else {
                ESP_LOGI(TAG, "Card scanned: %d bits, raw=0x%0*llX",
                         bit_count, (bit_count + 3) / 4, (unsigned long long)data);
                /* Not a frame we can decode — still worth recording, since a
                 * misconfigured reader shows up as a stream of these. */
                char raw_hex[17];
                snprintf(raw_hex, sizeof(raw_hex), "%0*llX",
                         (bit_count + 3) / 4, (unsigned long long)data);
                report_scan(raw_hex, bit_count,
                            card_scan_result_t_unknown_format, NULL, NULL);
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
