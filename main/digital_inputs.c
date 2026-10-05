#include "digital_inputs.h"
#include "config.h"
#include "event_manager.h"
#include "hw.h"
#include "mqtt.h"
#include "outputs.h"
#include "tca_io.h"
#include "webserver.h"

#include <stdio.h>

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

/* ── MQTT publishing ────────────────────────────────────────────────────── */

/* Raw per-slot level. Published for every mode, including `none`, so an input
 * is observable over MQTT regardless of what it is wired to. */
static void publish_input_level(int idx, bool logical_level)
{
    char subtopic[32];
    snprintf(subtopic, sizeof(subtopic), "input/inp%d/state", idx + 1);
    mqtt_publish(subtopic, logical_level ? "ON" : "OFF", 0, true);
}

/* Semantic aliases for the sensor modes, in the vocabulary Home Assistant
 * expects for a door / lock binary_sensor. */
static void publish_semantic_state(int mode, bool logical_level)
{
    switch (mode) {
    case digital_input_mode_t_door_close_sensor:
        mqtt_publish("door/state", logical_level ? "CLOSED" : "OPEN", 0, true);
        break;
    case digital_input_mode_t_door_lock_sensor:
        mqtt_publish("lock/state", logical_level ? "LOCKED" : "UNLOCKED", 0, true);
        break;
    default:
        break;
    }
}

/* ── Action dispatch ────────────────────────────────────────────────────── */

/* Returns true when the change affects the status payload, so the caller can
 * push once the shared sensor state has been recomputed. */
static bool handle_input_change(int idx, int mode, bool logical_level)
{
    publish_input_level(idx, logical_level);
    publish_semantic_state(mode, logical_level);

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
            event_emit_push_to_exit(idx + 1);
        }
        break;

    /* The status push for these is deferred to the end of the poll cycle:
     * webserver_push_status() reads the shared sensor state, which this cycle
     * has not recomputed yet, so pushing here would send a stale payload. */
    case digital_input_mode_t_door_close_sensor:
        ESP_LOGI(TAG, "INP%d: door %s", idx + 1, logical_level ? "CLOSED" : "OPEN");
        event_emit_door_state(logical_level);
        return true;

    case digital_input_mode_t_door_lock_sensor:
        ESP_LOGI(TAG, "INP%d: lock %s", idx + 1, logical_level ? "LOCKED" : "UNLOCKED");
        event_emit_lock_state(logical_level);
        return true;

    /* Reporting only — the MQTT publish above is the whole point of this mode. */
    case digital_input_mode_t_generic:
        ESP_LOGI(TAG, "INP%d: %s", idx + 1, logical_level ? "ON" : "OFF");
        break;

    default:
        break;
    }

    return false;
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
        bool status_dirty    = false;

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
                    if (handle_input_change(i, mode, inverted ? !raw : raw))
                        status_dirty = true;
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

        /* Push after the shared state is current, and only once per cycle even
         * if both sensors moved together. */
        if (status_dirty)
            webserver_push_status();
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

bool digital_inputs_door_close_state(bool *closed)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool configured = s_door_close_configured;
    if (configured) *closed = s_door_close_closed;
    xSemaphoreGive(s_state_mutex);
    return configured;
}

bool digital_inputs_door_lock_state(bool *locked)
{
    if (!s_state_mutex) return false;
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    bool configured = s_door_lock_configured;
    if (configured) *locked = s_door_lock_locked;
    xSemaphoreGive(s_state_mutex);
    return configured;
}

void digital_inputs_publish_all_states(void)
{
    for (int i = 0; i < (int)INPUT_COUNT; i++) {
        /* confirmed_level is only written by the poll task; a torn read is not
         * possible for a bool and a stale value is corrected on the next edge. */
        if (!s_rt[i].initialized) continue;

        config_lock();
        const struct digital_input_config_t *cfg = input_cfg(i);
        int  mode     = cfg->mode;
        bool inverted = cfg->inverted;
        config_unlock();

        bool logical = inverted ? !s_rt[i].confirmed_level : s_rt[i].confirmed_level;
        publish_input_level(i, logical);
        publish_semantic_state(mode, logical);
    }
}

void digital_inputs_init(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    xTaskCreate(digital_inputs_task, "dig_inputs", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Digital inputs initialised (%d input(s))", (int)INPUT_COUNT);
}
