#pragma once

#include <stdbool.h>

typedef enum {
    OUTPUT_RELAY      = 0,
    OUTPUT_LED_RED    = 1,
    OUTPUT_LED_GREEN  = 2,
    OUTPUT_BEEPER     = 3,
    OUTPUT_LED_READER = 4,
    OUTPUT_COUNT      = 5,
} output_id_t;

/* Initialise output pins and register IP event handlers for green LED. */
void outputs_init(void);

/* Re-evaluate and apply the base state for all idle outputs.
 * Green LED tracks whether the device has an IP address. */
void outputs_update_base_state(void);

/* Play a timed pattern on an output.
 *   on_ms   - duration the output is active per repeat
 *   off_ms  - gap between repeats (ignored when repeats == 1)
 *   repeats - number of on/off cycles
 * Cancels any pattern currently running on that output. */
void outputs_play_pattern(output_id_t id, int on_ms, int off_ms, int repeats);

/* Invert the output's base state for ms milliseconds, then return to base.
 * Used to produce a visible "blink" on outputs that are ON by default
 * (e.g. green LED while IP is present) or to flash outputs that are OFF. */
void outputs_flash_invert(output_id_t id, int ms);
