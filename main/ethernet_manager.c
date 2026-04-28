#include "ethernet_manager.h"
#include "hw.h"

#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "eth";

static esp_netif_t *s_eth_netif = NULL;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "Link up");   break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Started");   break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Stopped");   break;
    default: break;
    }
}

static void got_ip_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&ev->ip_info.ip));
}

esp_err_t ethernet_manager_init(void)
{
    /* MAC config */
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();

    /* PHY config */
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr       = HW_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = HW_ETH_PHY_RST_GPIO;

    /* ESP32 internal EMAC config */
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num  = HW_ETH_MDC_GPIO;
    emac_cfg.smi_gpio.mdio_num = HW_ETH_MDIO_GPIO;
    /* RTL8201F drives the 50 MHz RMII reference clock on its REFCLKO pin;
     * ESP32 receives it on GPIO0. */
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = HW_ETH_RMII_CLK_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    if (!mac) {
        ESP_LOGE(TAG, "Failed to create MAC instance");
        return ESP_FAIL;
    }

    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    if (!phy) {
        ESP_LOGE(TAG, "Failed to create PHY instance");
        mac->del(mac);
        return ESP_FAIL;
    }

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_cfg, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet driver install failed");
        mac->del(mac);
        phy->del(phy);
        return ESP_FAIL;
    }

    /* Create netif */
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(s_eth_netif, glue));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID,
                                               eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                               got_ip_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return ESP_OK;
}

esp_netif_t *ethernet_manager_get_netif(void)
{
    return s_eth_netif;
}
