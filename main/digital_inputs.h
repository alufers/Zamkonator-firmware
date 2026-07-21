#pragma once

#include <stdbool.h>

/* Initialise and start the digital-input polling task. */
void digital_inputs_init(void);

/* Query current state of the door-close sensor.
 * Returns true if any input is configured as DOOR_CLOSE_SENSOR, false otherwise.
 * When true, *closed is set: true = CLOSED, false = OPEN.
 * If several inputs share the mode, the highest-numbered one wins. */
bool digital_inputs_door_close_state(bool *closed);

/* Query current state of the door-lock sensor.
 * Returns true if any input is configured as DOOR_LOCK_SENSOR, false otherwise.
 * When true, *locked is set: true = LOCKED, false = UNLOCKED.
 * If several inputs share the mode, the highest-numbered one wins. */
bool digital_inputs_door_lock_state(bool *locked);
