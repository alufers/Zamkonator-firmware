#pragma once

#include <stdbool.h>

/* Returns true when the auth proxy last responded with HTTP 200 to /users/-/stats,
 * or when no auth_proxy_base_url is configured (auth not needed).
 * Defaults to false until the first health check completes. */
bool auth_proxy_is_healthy(void);

/* Start the background health-check task. Must be called after outputs_init(). */
void auth_proxy_monitor_init(void);

/* Wake the monitor task immediately to run a health check.
 * Safe to call from any task or event handler. */
void auth_proxy_trigger_check(void);
