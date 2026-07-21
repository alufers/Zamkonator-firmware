#include "digital_inputs.h"
#include "config.h"
#include "hw.h"
#include "outputs.h"
#include "tca_io.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "json.gen.h"

static const char *TAG = "digital_inputs";

#define POLL_MS 10

/* Hardware pin assignments for each logical input slot. */
static const uint8_t k_hw_pins[] = {
    HW_TCA_INP1_PIN,
    HW_TCA_INP2_PIN,
    HW_TCA_INP3_PIN,
};
#define INPUT_COUNT (sizeof(k_hw_pins) / sizeof(k_hw_pins[0]))

/* Config block backing each slot. Caller must hold the config lock. */
static struct digital_input_config_t *input_cfg(int idx)
{
    switch (idx) {
    case 0:  return &g_config.input_inp1;
    case 1:  return &g_config.input_inp2;
    case 2:  return &g_config.input_inp3;
    default: return NULL;
    }
}

typedef struct {
    bool raw_level;
    bool pending_level;
    int64_t  pending_since_us;
    bool confirmed_level;
    bool initialized;
} input_rt_t;

static input_rt_t s_rt[INPUT_COUNT];

/* ── Shared state for status endpoint ───────────────────────────────────── */

static SemaphoreHandle_t s_state_mutex;
static bool s_door_close_configured;
static bool s_door_close_closed;
static bool s_door_lock_configured;
static bool s_door_lock_locked;

/* ── Action dispatch ────────────────────────────────────────────────────── */

static void handle_input_change(int idx, int mode, bool logical_level)
{
    switch (mode) {
    case digital_input_mode_t_push_to_exit:
        if (!logical_level) break;
        {
            int relay_open_ms;
            config_lock();
            relay_open_ms = g_config.relay_open_ms;
            config_unlock();
            ESP_LOGI(TAG, "INP%d: push-to-exit triggered, opening relay for %d ms",
                     idx + 1, relay_open_ms);
            outputs_play_pattern(OUTPUT_RELAY,      relay_open_ms, 0, 1);
            outputs_play_pattern(OUTPUT_LED_READER, relay_open_ms, 0, 1);
        }
        break;

    /* Sensor modes only need logging here: the poll loop republishes their
     * state to the status endpoint every cycle. */
    case digital_input_mode_t_door_close_sensor:
        ESP_LOGI(TAG, "INP%d: door %s", idx + 1, logical_level ? "CLOSED" : "OPEN");
        break;

    case digital_input_mode_t_door_lock_sensor:
        ESP_LOGI(TAG, "INP%d: lock %s", idx + 1, logical_level ? "LOCKED" : "UNLOCKED");
        break;

    default:
        break;
    }
}

/* ── Polling task ───────────────────────────────────────────────────────── */

static void digital_inputs_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));

        uint16_t port_val = 0;
        if (tca_io_port_read(&port_val) != ESP_OK)
            continue;

        int64_t now_us = esp_timer_get_time();

        /* Sensor state is recomputed from scratch each cycle, so a slot that is
         * not a sensor simply contributes nothing — it can no longer clobber
         * the state another slot is publishing. */
        bool door_close_seen = false, door_close_closed = false;
        bool door_lock_seen  = false, door_lock_locked  = false;

        for (int i = 0; i < (int)INPUT_COUNT; i++) {
            /* Snapshot per-input config */
            config_lock();
            const struct digital_input_config_t *cfg = input_cfg(i);
            int  mode        = cfg->mode;
            bool inverted    = cfg->inverted;
            int  debounce_ms = cfg->debounce_ms;
            config_unlock();

            bool raw = (port_val >> k_hw_pins[i]) & 1;
            input_rt_t *rt = &s_rt[i];

            if (!rt->initialized) {
                /* First cycle: silently latch current state as baseline */
                rt->raw_level        = raw;
                rt->pending_level    = raw;
                rt->confirmed_level  = raw;
                rt->pending_since_us = now_us;
                rt->initialized      = true;
            } else {
                /* Debounce: reset timer whenever raw level changes */
                if (raw != rt->pending_level) {
                    rt->pending_level    = raw;
                    rt->pending_since_us = now_us;
                }

                /* Confirm when stable long enough */
                if (raw != rt->confirmed_level &&
                    (now_us - rt->pending_since_us) >= (int64_t)debounce_ms * 1000) {
                    rt->confirmed_level = raw;
                    handle_input_change(i, mode, inverted ? !raw : raw);
                }
            }

            bool logical = inverted ? !rt->confirmed_level : rt->confirmed_level;
            if (mode == digital_input_mode_t_door_close_sensor) {
                door_close_seen   = true;
                door_close_closed = logical;
            } else if (mode == digital_input_mode_t_door_lock_sensor) {
                door_lock_seen   = true;
                door_lock_locked = logical;
            }
        }

        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_door_close_configured = door_close_seen;
        s_door_close_closed     = door_close_closed;
        s_door_lock_configured  = door_lock_seen;
        s_door_lock_locked      = door_lock_locked;
        xSemaphoreGive(s_state_mutex);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool digital_inputs_door_close_state(bool *closed)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool configured = s_door_close_configured;
    if (configured) *closed = s_door_close_closed;
    xSemaphoreGive(s_state_mutex);
    return configured;
}

bool digital_inputs_door_lock_state(bool *locked)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool configured = s_door_lock_configured;
    if (configured) *locked = s_door_lock_locked;
    xSemaphoreGive(s_state_mutex);
    return configured;
}

void digital_inputs_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    xTaskCreate(digital_inputs_task, "dig_inputs", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Digital inputs initialised (%d input(s))", (int)INPUT_COUNT);
}
