#pragma once

/*
 * Hardware pin definitions for Zamkonator.
 * All assignments are hardcoded — do not expose via settings.
 */

/* ── Ethernet: built-in EMAC + RTL8201F PHY ─────────────────────────────── */

#define HW_ETH_MDC_GPIO      16   /* SMI clock       */
#define HW_ETH_MDIO_GPIO     17   /* SMI data        */
#define HW_ETH_PHY_RST_GPIO   4   /* PHY reset (active-low) */
#define HW_ETH_PHY_ADDR       0   /* MDIO address of RTL8201F on this board */

/* RMII reference clock input on GPIO0.
 * The RTL8201F REFCLKO pin (or an external 50 MHz oscillator) must drive this.
 * If the ESP32 must generate the clock instead, change clock_mode in
 * ethernet_manager.c to EMAC_CLK_OUT and set this to 16 or 17 (inverted). */
#define HW_ETH_RMII_CLK_GPIO  0

/* ── SD card: SPI mode via SPI2 (HSPI) ──────────────────────────────────── */

#define HW_SD_CS_GPIO    13   /* DAT3/nCS */
#define HW_SD_MOSI_GPIO  15   /* CMD      */
#define HW_SD_CLK_GPIO   14   /* CLK      */
#define HW_SD_MISO_GPIO   2   /* DAT0     */
