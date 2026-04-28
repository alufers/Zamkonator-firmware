#include "mqtt.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "mqtt_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "mqtt";

/* ── Global status ───────────────────────────────────────────────────────── */

int g_mqtt_status = mqtt_status_t_unconfigured;

/* ── Internal state ──────────────────────────────────────────────────────── */

static esp_mqtt_client_handle_t s_client = NULL;
static SemaphoreHandle_t        s_mutex;

typedef struct {
    bool  enabled;
    char  broker[128];
    int   port;
    char  username[64];
    char  password[128];
} mqtt_active_cfg_t;

static mqtt_active_cfg_t s_active_cfg;

/* ── MQTT event handler ──────────────────────────────────────────────────── */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                                int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Connected to broker");
        g_mqtt_status = mqtt_status_t_connected;
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Disconnected from broker");
        g_mqtt_status = mqtt_status_t_disconnected;
        break;

    case MQTT_EVENT_ERROR: {
        esp_mqtt_event_handle_t ev = (esp_mqtt_event_handle_t)event_data;
        ESP_LOGW(TAG, "MQTT error: type=%d", (int)ev->error_handle->error_type);
        break;
    }

    default:
        break;
    }
}

/* ── Init / deinit helpers ───────────────────────────────────────────────── */

static void mqtt_do_deinit(void)
{
    if (!s_client) return;
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = NULL;
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));
}

static void mqtt_do_init(const mqtt_active_cfg_t *cfg)
{
    if (!cfg->enabled || cfg->broker[0] == '\0') {
        g_mqtt_status = mqtt_status_t_unconfigured;
        return;
    }

    char uri[192];
    snprintf(uri, sizeof(uri), "mqtt://%s:%d", cfg->broker, cfg->port);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri  = uri,
        .session.keepalive   = 60,
        .task.stack_size     = 6000,
        .buffer.size         = 2048,
    };

    if (cfg->username[0]) {
        mqtt_cfg.credentials.username                = cfg->username;
        mqtt_cfg.credentials.authentication.password = cfg->password;
    }

    s_client = esp_mqtt_client_init(&mqtt_cfg);
    if (!s_client) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        g_mqtt_status = mqtt_status_t_disconnected;
        return;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);
    g_mqtt_status = mqtt_status_t_connecting;
    s_active_cfg  = *cfg;
    ESP_LOGI(TAG, "MQTT client started → %s", uri);
}

/* ── Public API ──────────────────────────────────────────────────────────── */

void mqtt_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    g_mqtt_status = mqtt_status_t_unconfigured;
    memset(&s_active_cfg, 0, sizeof(s_active_cfg));

    mqtt_active_cfg_t cfg = {0};
    config_lock();
    cfg.enabled = (bool)g_config.mqtt.enabled;
    cfg.port    = g_config.mqtt.port;
    strlcpy(cfg.broker,   sstr_cstr(g_config.mqtt.broker),   sizeof(cfg.broker));
    strlcpy(cfg.username, sstr_cstr(g_config.mqtt.username), sizeof(cfg.username));
    strlcpy(cfg.password, sstr_cstr(g_config.mqtt.password), sizeof(cfg.password));
    config_unlock();

    mqtt_do_init(&cfg);
}

void mqtt_apply_config(void)
{
    mqtt_active_cfg_t cfg = {0};
    config_lock();
    cfg.enabled = (bool)g_config.mqtt.enabled;
    cfg.port    = g_config.mqtt.port;
    strlcpy(cfg.broker,   sstr_cstr(g_config.mqtt.broker),   sizeof(cfg.broker));
    strlcpy(cfg.username, sstr_cstr(g_config.mqtt.username), sizeof(cfg.username));
    strlcpy(cfg.password, sstr_cstr(g_config.mqtt.password), sizeof(cfg.password));
    config_unlock();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool changed = (memcmp(&s_active_cfg, &cfg, sizeof(cfg)) != 0);
    if (changed) {
        ESP_LOGI(TAG, "MQTT config changed, reinitialising");
        mqtt_do_deinit();
        mqtt_do_init(&cfg);
    }
    xSemaphoreGive(s_mutex);
}
