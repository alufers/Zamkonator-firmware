#pragma once

#include "esp_err.h"

#define SDCARD_MOUNT_POINT "/sdcard"

/* Mount the SD card via SPI and list the root directory.
 * Must be called after webserver_early_init() (uses gtw_console_log). */
esp_err_t sdcard_init(void);
