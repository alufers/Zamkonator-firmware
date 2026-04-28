#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Initialise i2cdev subsystem and the TCA9555 descriptor.
 * Must be called once before any task that reads TCA pins. */
esp_err_t tca_io_init(void);

/* Set an output pin level. Pin must have been configured as output in tca_io_init(). */
esp_err_t tca_io_set_level(uint8_t pin, bool level);

/* Read all 16 port bits at once (efficient for polling multiple inputs). */
esp_err_t tca_io_port_read(uint16_t *val);

/* Read a single input pin level. */
esp_err_t tca_io_read_input(uint8_t pin, bool *level);

/* Read SD card detect pin (TCA P1.4, driver index 12).
 * Sets *present = true when card is physically inserted (active-low pin). */
esp_err_t tca_io_read_sd_detect(bool *present);
