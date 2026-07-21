#pragma once

/* Initialise coredump-to-SD subsystem.
 * Must be called after esp_event_loop_create_default(). */
void coredump_saver_init(void);
