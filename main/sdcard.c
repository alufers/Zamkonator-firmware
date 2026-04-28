#include "sdcard.h"
#include "hw.h"
#include "webserver.h"

#include <dirent.h>
#include <string.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"

static const char *TAG = "sdcard";

esp_err_t sdcard_init(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed  = false,
        .max_files               = 5,
        .allocation_unit_size    = 16 * 1024,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.unaligned_multi_block_rw_max_chunk_size = 8;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num   = HW_SD_MOSI_GPIO,
        .miso_io_num   = HW_SD_MISO_GPIO,
        .sclk_io_num   = HW_SD_CLK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = HW_SD_CS_GPIO;
    slot_cfg.host_id = host.slot;

    sdmmc_card_t *card;
    ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &host, &slot_cfg,
                                  &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(host.slot);
        return ret;
    }

    sdmmc_card_print_info(stdout, card);
    gtw_console_log("SD card mounted at " SDCARD_MOUNT_POINT
                    " (%llu MB)",
                    (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));

    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open root directory");
        return ESP_OK;
    }

    gtw_console_log("SD card contents:");
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        gtw_console_log("  %s %s",
                        ent->d_type == DT_DIR ? "[DIR]" : "     ",
                        ent->d_name);
    }
    closedir(dir);

    return ESP_OK;
}
