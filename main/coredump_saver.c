#include "coredump_saver.h"
#include "sdcard.h"
#include "utils.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_core_dump.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_timer.h"

static const char *TAG = "coredump_saver";

#define COREDUMP_DIR    "/sdcard/coredumps"
#define CHUNK_SIZE      4096
#define RETRY_INTERVAL_US (60ULL * 1000000ULL)  /* 60 s */

static esp_timer_handle_t s_timer;
static bool               s_sntp_started = false;
static bool               s_save_done    = false;  /* attempt only once per boot */

static void do_save_coredump(void)
{
    if (!sdcard_is_mounted()) {
        ESP_LOGW(TAG, "SD card not mounted, skipping coredump save");
        return;
    }

    size_t flash_addr = 0;
    size_t size       = 0;
    esp_err_t err = esp_core_dump_image_get(&flash_addr, &size);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No pending coredump (%s)", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "Coredump found: addr=0x%zx size=%zu bytes", flash_addr, size);

    /* Ensure destination directory exists */
    if (mkdir(COREDUMP_DIR, 0755) != 0 && errno != EEXIST) {
        ESP_LOGE(TAG, "Failed to create %s: %d", COREDUMP_DIR, errno);
        return;
    }

    /* Build timestamped filename */
    char path[64];
    time_t now = time(NULL);
    struct tm tm_info;
    gmtime_r(&now, &tm_info);
    strftime(path, sizeof(path), COREDUMP_DIR "/%Y%m%d_%H%M%S.bin", &tm_info);

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s for writing: %d", path, errno);
        return;
    }

    uint8_t *buf = malloc(CHUNK_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "Failed to allocate read buffer");
        fclose(f);
        remove(path);
        return;
    }

    uint32_t offset = 0;
    bool ok = true;
    while (offset < size) {
        uint32_t to_read = (size - offset < CHUNK_SIZE) ? (size - offset) : CHUNK_SIZE;
        err = esp_flash_read(esp_flash_default_chip, buf, flash_addr + offset, to_read);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Flash read at offset %"PRIu32" failed: %s", offset, esp_err_to_name(err));
            ok = false;
            break;
        }
        if (fwrite(buf, 1, to_read, f) != to_read) {
            ESP_LOGE(TAG, "File write failed at offset %"PRIu32, offset);
            ok = false;
            break;
        }
        offset += to_read;
    }

    fclose(f);
    free(buf);

    if (!ok) {
        remove(path);
        ESP_LOGE(TAG, "Coredump save failed — leaving in flash for next boot");
        return;
    }

    ESP_LOGI(TAG, "Coredump saved to %s", path);
    esp_core_dump_image_erase();
}

static void timer_cb(void *arg)
{
    if (s_save_done)
        return;

    if (!utils_time_is_valid()) {
        ESP_LOGW(TAG, "NTP time not yet available, retrying in 60 s");
        esp_timer_start_once(s_timer, RETRY_INTERVAL_US);
        return;
    }

    s_save_done = true;
    do_save_coredump();
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                            int32_t event_id, void *event_data)
{
    if (!s_sntp_started) {
        esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
        esp_sntp_setservername(0, "pool.ntp.org");
        esp_sntp_init();
        s_sntp_started = true;
        ESP_LOGI(TAG, "SNTP started");
    }

    /* (Re-)arm the timer: fire 60 s after getting an IP */
    esp_timer_stop(s_timer);
    esp_timer_start_once(s_timer, RETRY_INTERVAL_US);
    ESP_LOGI(TAG, "Coredump save timer armed (60 s)");
}

void coredump_saver_init(void)
{
    const esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name     = "coredump_save",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_timer));

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_handler, NULL));
}
