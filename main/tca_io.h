#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Initialise i2cdev subsystem and the TCA9555 descriptor.
 * Must be called once before any task that reads TCA pins. */
esp_err_t tca_io_init(void);

/* Read SD card detect pin (TCA P1.4, driver index 12).
 * Sets *present = true when card is physically inserted (active-low pin). */
esp_err_t tca_io_read_sd_detect(bool *present);
