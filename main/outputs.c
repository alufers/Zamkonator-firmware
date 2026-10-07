#include "outputs.h"
#include "auth_proxy.h"
#include "tca_io.h"
#include "hw.h"
#include "ethernet_manager.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_event.h"
#include "esp_netif.h"

static const char *TAG = "outputs";

static const uint8_t k_pins[OUTPUT_COUNT] = {
    [OUTPUT_RELAY]      = HW_TCA_RELAY_PIN,
    [OUTPUT_LED_RED]    = HW_TCA_STATUS_RED_PIN,
    [OUTPUT_LED_GREEN]  = HW_TCA_STATUS_GREEN_PIN,
    [OUTPUT_BEEPER]     = HW_TCA_BEEPER_PIN,
    [OUTPUT_LED_READER] = HW_TCA_READER_LED_PIN,
};

typedef struct {
    esp_timer_handle_t timer;
    output_id_t        id;
    int                on_ms;
    int                off_ms;
    int                repeats_left;
    bool               in_on_phase;
    bool               inverted;   /* true → pin starts at !base, returns to base */
} output_state_t;

static output_state_t s_states[OUTPUT_COUNT];

/* ── Base state ─────────────────────────────────────────────────────────── */

static bool has_ip(void)
{
    esp_netif_t *netif = ethernet_manager_get_netif();
    if (!netif) return false;
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(netif, &info) != ESP_OK) return false;
    return info.ip.addr != 0;
}

static bool base_level(output_id_t id)
{
    switch (id) {
    case OUTPUT_LED_GREEN: return has_ip();
    case OUTPUT_LED_RED:   return !auth_proxy_is_healthy();
    default:               return false;
    }
}

void outputs_update_base_state(void)
{
    for (int i = 0; i < OUTPUT_COUNT; i++) {
        if (s_states[i].repeats_left == 0) {
            tca_io_set_level(k_pins[i], base_level((output_id_t)i));
        }
    }
}

/* ── Pattern timer ──────────────────────────────────────────────────────── */

static void pattern_timer_cb(void *arg)
{
    output_state_t *st = (output_state_t *)arg;

    if (st->in_on_phase) {
        /* Transition from on-phase: set to the "between repeats" level */
        bool between_level = st->inverted ? base_level(st->id) : false;
        tca_io_set_level(k_pins[st->id], between_level);
        st->in_on_phase = false;
        st->repeats_left--;
        if (st->repeats_left > 0 && st->off_ms > 0) {
            esp_timer_start_once(st->timer, (uint64_t)st->off_ms * 1000);
        } else {
            /* Pattern complete — restore base state */
            tca_io_set_level(k_pins[st->id], base_level(st->id));
        }
    } else {
        /* Start next on-phase */
        bool on_level = st->inverted ? !base_level(st->id) : true;
        tca_io_set_level(k_pins[st->id], on_level);
        st->in_on_phase = true;
        esp_timer_start_once(st->timer, (uint64_t)st->on_ms * 1000);
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void outputs_play_pattern(output_id_t id, int on_ms, int off_ms, int repeats)
{
    if (id >= OUTPUT_COUNT || repeats <= 0 || on_ms <= 0) return;

    output_state_t *st = &s_states[id];
    esp_timer_stop(st->timer);

    st->on_ms        = on_ms;
    st->off_ms       = off_ms;
    st->repeats_left = repeats;
    st->in_on_phase  = true;
    st->inverted     = false;

    tca_io_set_level(k_pins[id], true);
    esp_timer_start_once(st->timer, (uint64_t)on_ms * 1000);
}

void outputs_flash_invert(output_id_t id, int ms)
{
    if (id >= OUTPUT_COUNT || ms <= 0) return;

    output_state_t *st = &s_states[id];
    esp_timer_stop(st->timer);

    st->on_ms        = ms;
    st->off_ms       = 0;
    st->repeats_left = 1;
    st->in_on_phase  = true;
    st->inverted     = true;

    tca_io_set_level(k_pins[id], !base_level(id));
    esp_timer_start_once(st->timer, (uint64_t)ms * 1000);
}

bool outputs_is_active(output_id_t id)
{
    if (id >= OUTPUT_COUNT) return false;
    return s_states[id].repeats_left > 0;
}

void outputs_cancel(output_id_t id)
{
    if (id >= OUTPUT_COUNT) return;

    output_state_t *st = &s_states[id];
    esp_timer_stop(st->timer);

    st->repeats_left = 0;
    st->in_on_phase  = false;
    st->inverted     = false;

    tca_io_set_level(k_pins[id], base_level(id));
}

static void ip_event_handler(void *arg, esp_event_base_t base,
                             int32_t event_id, void *data)
{
    (void)arg; (void)base; (void)data;
    outputs_update_base_state();
    if (event_id == IP_EVENT_ETH_GOT_IP)
        auth_proxy_trigger_check();
}

void outputs_init(void)
{
    for (int i = 0; i < OUTPUT_COUNT; i++) {
        s_states[i].id           = (output_id_t)i;
        s_states[i].repeats_left = 0;
        s_states[i].in_on_phase  = false;
        s_states[i].inverted     = false;

        esp_timer_create_args_t args = {
            .callback        = pattern_timer_cb,
            .arg             = &s_states[i],
            .dispatch_method = ESP_TIMER_TASK,
        };
        esp_timer_create(&args, &s_states[i].timer);
    }

    /* Apply initial base state */
    outputs_update_base_state();

    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP,
                                ip_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_LOST_IP,
                                ip_event_handler, NULL);

    ESP_LOGI(TAG, "outputs initialised");
}
