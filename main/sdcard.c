#include "sdcard.h"
#include "tca_io.h"
#include "hw.h"

#include <dirent.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "sdcard";

static volatile bool  s_mounted = false;
static sdmmc_card_t  *s_card    = NULL;
static sdmmc_host_t   s_host;
static spi_host_device_t s_spi_slot;

bool sdcard_is_mounted(void)
{
    return s_mounted;
}

static void sdcard_list_root(void)
{
    DIR *dir = opendir(SDCARD_MOUNT_POINT);
    if (!dir) {
        ESP_LOGW(TAG, "Could not open root directory");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "  %s %s",
                 ent->d_type == DT_DIR ? "[DIR]" : "     ",
                 ent->d_name);
    }
    closedir(dir);
}

static void sdcard_mount(void)
{
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 5,
        .allocation_unit_size   = 16 * 1024,
    };

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = HW_SD_CS_GPIO;
    slot_cfg.host_id = s_spi_slot;

    sdmmc_card_t *card = NULL;
    esp_err_t ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT, &s_host,
                                             &slot_cfg, &mount_cfg, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
        return;
    }

    s_card    = card;
    s_mounted = true;

    sdmmc_card_print_info(stdout, card);
    ESP_LOGI(TAG, "SD card mounted at " SDCARD_MOUNT_POINT " (%llu MB)",
             (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    sdcard_list_root();
}

static void sdcard_unmount(void)
{
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    if (ret != ESP_OK)
        ESP_LOGW(TAG, "Unmount returned: %s", esp_err_to_name(ret));

    s_mounted = false;
    s_card    = NULL;
    ESP_LOGI(TAG, "SD card removed");
}

static void sdcard_task(void *arg)
{
    (void)arg;

    s_host = (sdmmc_host_t)SDSPI_HOST_DEFAULT();
    s_host.unaligned_multi_block_rw_max_chunk_size = 8;
    s_spi_slot = (spi_host_device_t)s_host.slot;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num     = HW_SD_MOSI_GPIO,
        .miso_io_num     = HW_SD_MISO_GPIO,
        .sclk_io_num     = HW_SD_CLK_GPIO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 4000,
    };

    esp_err_t ret = spi_bus_initialize(s_spi_slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s — SD card unavailable",
                 esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    int  tick         = 0;
    int  health_fails = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        tick++;

        bool present = false;
        if (tca_io_read_sd_detect(&present) != ESP_OK)
            continue;

        if (present && !s_mounted) {
            health_fails = 0;
            sdcard_mount();
        } else if (!present && s_mounted) {
            health_fails = 0;
            sdcard_unmount();
        } else if (s_mounted && (tick % 10 == 0)) {
            /* Health check every 5 s (10 × 500 ms) */
            if (sdmmc_get_status(s_card) != ESP_OK) {
                ESP_LOGW(TAG, "SD health check failed (%d/3)", ++health_fails);
                if (health_fails >= 3) {
                    ESP_LOGW(TAG, "SD card unresponsive, forcing remount");
                    sdcard_unmount();
                    health_fails = 0;
                    /* present && !s_mounted will be true next iteration → remount */
                }
            } else {
                health_fails = 0;
            }
        }
    }
}

void sdcard_init(void)
{
    xTaskCreate(sdcard_task, "sdcard", 16384, NULL, 5, NULL);
}
