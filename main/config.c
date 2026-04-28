#include "config.h"
#include "background_worker.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_mac.h"

static const char *TAG        = "config";
static const char *CONFIG_PATH = "/littlefs/config.json";

/* ── Global state ────────────────────────────────────────────────────────── */

struct gateway_config_t g_config;
SemaphoreHandle_t       g_config_mutex;

/* ── Default hostname ────────────────────────────────────────────────────── */

static void set_default_hostname(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_ETH);
    char buf[32];
    snprintf(buf, sizeof(buf), "zamkonator%02x%02x", mac[4], mac[5]);
    sstr_free(g_config.hostname);
    g_config.hostname = sstr(buf);
}

/* ── File I/O ────────────────────────────────────────────────────────────── */

void config_do_save(void)
{
    config_lock();
    sstr_t json = sstr_new();
    json_marshal_gateway_config_t(&g_config, json);
    config_unlock();

    FILE *f = fopen(CONFIG_PATH, "w");
    if (f) {
        fputs(sstr_cstr(json), f);
        fclose(f);
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGW(TAG, "Failed to open config for writing");
    }
    sstr_free(json);
}

static void do_load(void)
{
    FILE *f = fopen(CONFIG_PATH, "r");
    if (!f) {
        ESP_LOGI(TAG, "No config file found, using defaults");
        return;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 65536) { fclose(f); return; }

    char *buf = malloc((size_t)size + 1);
    if (!buf) { fclose(f); return; }
    fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[size] = '\0';

    config_lock();
    sstr_t in = sstr_of(buf, (size_t)size);
    int rc = json_unmarshal_gateway_config_t(in, &g_config);
    sstr_free(in);
    config_unlock();

    free(buf);

    if (rc != 0) {
        ESP_LOGW(TAG, "Config JSON parse error (rc=%d)", rc);
        return;
    }

    config_lock();
    ESP_LOGI(TAG, "Loaded config: hostname=%s", sstr_cstr(g_config.hostname));
    config_unlock();
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void config_init(void)
{
    g_config_mutex = xSemaphoreCreateMutex();

    gateway_config_t_init(&g_config);

    /* Set defaults */
    g_config.mqtt.port     = 1883;
    g_config.mqtt.broker   = sstr("");
    g_config.mqtt.username = sstr("");
    g_config.mqtt.password = sstr("");
    g_config.language  = language_t_en;
    g_config.web_password_enabled = 0;
    g_config.web_password    = sstr("");
    g_config.mqtt.mqtt_prefix = sstr("zamkonator");
    g_config.auth_proxy_base_url                   = sstr("");
    g_config.auth_proxy_timeout_ms                 = 10000;
    g_config.auth_proxy_healthcheck_interval_ms    = 30000;
    g_config.relay_open_ms                         = 8000;

    set_default_hostname();
    do_load();
}

void config_mark_dirty(void)
{
    background_worker_notify_save();
}

void config_save_now(void)
{
    background_worker_save_now();
}
