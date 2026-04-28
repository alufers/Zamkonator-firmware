#pragma once

#include "esp_err.h"

/* Current MQTT connection status (mqtt_status_t enum from json.gen.h). */
extern int g_mqtt_status;

/* Called once from app_main. */
void mqtt_init(void);

/* Called from webserver apply_settings to react to config changes at runtime.
 * Safe to call from any task (NOT from an MQTT event handler). */
void mqtt_apply_config(void);
