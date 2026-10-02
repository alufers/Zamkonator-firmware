#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* Current MQTT connection status (mqtt_status_t enum from json.gen.h). */
extern int g_mqtt_status;

/* Called once from app_main. */
void mqtt_init(void);

/* Called from webserver apply_settings to react to config changes at runtime.
 * Safe to call from any task (NOT from an MQTT event handler). */
void mqtt_apply_config(void);

/* Publish to "<mqtt_prefix>/<hostname>/<subtopic>".
 * No-op returning false when MQTT is disabled or not connected. */
bool mqtt_publish(const char *subtopic, const char *payload, int qos, bool retain);

/* Publish a marshalled event to "<prefix>/<hostname>/event/<type_name>".
 * Called by the event manager fan-out task. */
void mqtt_publish_event(const char *type_name, const char *json);
