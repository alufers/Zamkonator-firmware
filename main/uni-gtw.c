#include "freertos/FreeRTOS.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "mdns.h"

#include "background_worker.h"
#include "config.h"
#include "esp_littlefs.h"
#include "ethernet_manager.h"
#include "mqtt.h"
#include "sdcard.h"
#include "webserver.h"

static const char *TAG = "zamkonator";

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    esp_vfs_littlefs_conf_t conf = {
        .base_path             = "/littlefs",
        .partition_label       = "littlefs",
        .format_if_mount_failed = true,
        .dont_mount            = false,
    };

    ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format filesystem");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find LittleFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize LittleFS (%s)",
                     esp_err_to_name(ret));
        }
        return;
    }

    /* Load persisted config */
    config_init();

    /* Background worker: deferred saves */
    background_worker_init();

    /* Init network stack before anything that opens sockets */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    webserver_early_init();
    webserver_start();

    /* Start Ethernet */
    ESP_ERROR_CHECK(ethernet_manager_init());

    /* Apply hostname to Ethernet netif and mDNS */
    char hostname[64];
    config_lock();
    snprintf(hostname, sizeof(hostname), "%s", sstr_cstr(g_config.hostname));
    config_unlock();

    esp_netif_t *eth_netif = ethernet_manager_get_netif();
    if (eth_netif)
        esp_netif_set_hostname(eth_netif, hostname);

    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_instance_name_set("Zamkonator");
    mdns_service_add(NULL, "_http",        "_tcp", 80, NULL, 0);
    mdns_service_add(NULL, "_zamkonator",  "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);

    /* MQTT init */
    mqtt_init();

    /* Mount SD card and log contents */
    sdcard_init();

    webserver_start_status_timer();
}
