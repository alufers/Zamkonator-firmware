#include "ethernet_manager.h"
#include "hw.h"

#include "esp_eth.h"
#include "esp_eth_driver.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "json.gen.h"

#include <stdlib.h>

static const char *TAG = "eth";

static esp_netif_t     *s_eth_netif  = NULL;
static esp_eth_handle_t s_eth_handle = NULL;

static volatile bool    s_link_up;
static volatile int     s_link_down_count;
static volatile int64_t s_link_up_since_us;
static volatile int64_t s_ip_acquired_at_us = -1;

static void eth_event_handler(void *arg, esp_event_base_t base,
                              int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Link up");
        s_link_up_since_us = esp_timer_get_time();
        s_link_up          = true;
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Link down");
        if (s_link_up) s_link_down_count++;
        s_link_up = false;
        break;
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
    s_ip_acquired_at_us = esp_timer_get_time();
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

    s_eth_handle = eth_handle;
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
    return ESP_OK;
}

esp_netif_t *ethernet_manager_get_netif(void)
{
    return s_eth_netif;
}

static sstr_t ip4_to_sstr(const esp_ip4_addr_t *addr)
{
    char buf[16];
    snprintf(buf, sizeof(buf), IPSTR, IP2STR(addr));
    return sstr(buf);
}

void ethernet_manager_get_status(struct network_status_t *out)
{
    esp_netif_t *netif = s_eth_netif;

    out->link_up         = s_link_up;
    out->link_down_count = s_link_down_count;
    if (out->link_up) {
        out->has_link_up_since = true;
        out->link_up_since     = s_link_up_since_us / 1000000LL;
    }

    if (out->link_up && s_eth_handle) {
        eth_speed_t speed;
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_SPEED, &speed) == ESP_OK) {
            out->has_speed_mbps = true;
            out->speed_mbps     = speed == ETH_SPEED_100M ? 100
                                : speed == ETH_SPEED_10M  ? 10
                                : 1000;
        }
        eth_duplex_t duplex;
        if (esp_eth_ioctl(s_eth_handle, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
            out->has_full_duplex = true;
            out->full_duplex     = duplex == ETH_DUPLEX_FULL;
        }
    }

    if (!netif) {
        out->hostname = sstr("");
        out->mac      = sstr("");
        out->ip       = sstr("");
        out->netmask  = sstr("");
        out->gateway  = sstr("");
        return;
    }

    const char *hostname = NULL;
    out->hostname = sstr(esp_netif_get_hostname(netif, &hostname) == ESP_OK && hostname
                         ? hostname : "");

    uint8_t mac[6] = {0};
    char    mac_str[18];
    esp_netif_get_mac(netif, mac);
    snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    out->mac = sstr(mac_str);

    esp_netif_dhcp_status_t dhcp;
    out->dhcp_enabled = esp_netif_dhcpc_get_status(netif, &dhcp) == ESP_OK &&
                        dhcp == ESP_NETIF_DHCP_STARTED;

    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(netif, &ip_info);
    out->got_ip  = ip_info.ip.addr != 0;
    out->ip      = ip4_to_sstr(&ip_info.ip);
    out->netmask = ip4_to_sstr(&ip_info.netmask);
    out->gateway = ip4_to_sstr(&ip_info.gw);
    if (out->got_ip && s_ip_acquired_at_us >= 0) {
        out->has_ip_acquired_at = true;
        out->ip_acquired_at     = s_ip_acquired_at_us / 1000000LL;
    }

    static const esp_netif_dns_type_t dns_types[] = {
        ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK,
    };
    out->dns = calloc(sizeof(dns_types) / sizeof(dns_types[0]), sizeof(sstr_t));
    for (size_t i = 0; out->dns && i < sizeof(dns_types) / sizeof(dns_types[0]); i++) {
        esp_netif_dns_info_t dns;
        if (esp_netif_get_dns_info(netif, dns_types[i], &dns) != ESP_OK) continue;
        if (dns.ip.type != ESP_IPADDR_TYPE_V4 || dns.ip.u_addr.ip4.addr == 0) continue;
        out->dns[out->dns_len++] = ip4_to_sstr(&dns.ip.u_addr.ip4);
    }

#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ip6[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int n6 = esp_netif_get_all_ip6(netif, ip6);
    if (n6 > 0) out->ipv6 = calloc(n6, sizeof(sstr_t));
    for (int i = 0; out->ipv6 && i < n6; i++) {
        char buf[40];
        snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(ip6[i]));
        out->ipv6[out->ipv6_len++] = sstr(buf);
    }
#endif
}
