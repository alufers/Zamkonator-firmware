#pragma once

/**
 * @brief Initialise the background worker task.
 *
 * Must be called after config_init().
 * Creates a FreeRTOS one-shot timer for debounced config saves (5 s).
 */
void background_worker_init(void);

/**
 * @brief Schedule a deferred save (resets the 5 s debounce timer).
 *
 * Safe to call from any task or ISR context.
 */
void background_worker_notify_save(void);

/**
 * @brief Perform an immediate synchronous save on the calling task.
 *
 * Cancels the debounce timer and calls config_do_save() directly.
 * Use before a planned reboot so dirty state is not lost.
 */
void background_worker_save_now(void);
