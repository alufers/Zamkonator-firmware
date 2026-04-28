#pragma once

#include <stdbool.h>

#define SDCARD_MOUNT_POINT "/sdcard"

/* Spawn the SD card hotplug task. Must be called after tca_io_init(). */
void sdcard_init(void);

/* Returns true if the SD card FAT volume is currently mounted. Thread-safe. */
bool sdcard_is_mounted(void);
