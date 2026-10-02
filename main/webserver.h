#pragma once

#include <stdint.h>
#include <stdbool.h>

void webserver_early_init(void);
void webserver_start(void);

/* Broadcast a raw JSON string to every connected WebSocket client.
 * Safe to call from any task; sending is deferred onto the httpd work queue. */
void webserver_ws_broadcast_json(const char *json);

/* Build a status message from the current device state and broadcast it.
 * Called whenever something the status payload reflects changes, so the UI
 * never has to poll. No-op when no clients are connected. */
void webserver_push_status(void);
