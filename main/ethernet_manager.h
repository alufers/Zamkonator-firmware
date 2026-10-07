#pragma once

#include "esp_err.h"
#include "esp_netif.h"

struct network_status_t;

/* Initialise Ethernet: MAC, PHY, netif, event handlers, start driver.
 * Must be called after esp_netif_init() and esp_event_loop_create_default(). */
esp_err_t ethernet_manager_init(void);

/* Returns the esp_netif created for Ethernet (or NULL before init). */
esp_netif_t *ethernet_manager_get_netif(void);

void ethernet_manager_get_status(struct network_status_t *out);
