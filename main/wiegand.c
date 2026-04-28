#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include <inttypes.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "hw.h"
#include "wiegand.h"

#define WIEGAND_MAX_BITS    64
#define WIEGAND_TIMEOUT_MS  25
/* Reject pulses within 500 µs of the previous one — filters optocoupler ringing.
 * Wiegand bit period is ~2 ms, so this is well within the valid range. */
#define WIEGAND_DEBOUNCE_US 500

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
            if (bit_count == 34) {
                /* Wiegand 34: 1 even parity + 32 data bits + 1 odd parity.
                 * Data bits are MSB-first; MIFARE UID bytes are in reverse
                 * order, so byte-swap to recover the card UID. */
                uint32_t raw      = (uint32_t)((data >> 1) & 0xFFFFFFFF);
                uint32_t mifare_id = __builtin_bswap32(raw);
                ESP_LOGI(TAG, "Card scanned: mifare_id=%08" PRIx32, mifare_id);
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

    xTaskCreate(wiegand_task, "wiegand", 2048, NULL, 5, NULL);
    ESP_LOGI(TAG, "Wiegand reader initialised on DAT0=GPIO%d DAT1=GPIO%d",
             HW_WIEGAND_DAT0_GPIO, HW_WIEGAND_DAT1_GPIO);
}
