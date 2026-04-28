#include "background_worker.h"
#include "config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"

static TimerHandle_t s_save_timer = NULL; /* one-shot, 5 s debounce */

static void save_timer_cb(TimerHandle_t t)
{
    (void)t;
    config_do_save();
}

void background_worker_init(void)
{
    s_save_timer = xTimerCreate("cfg_save", pdMS_TO_TICKS(5000),
                                pdFALSE, NULL, save_timer_cb);
}

void background_worker_notify_save(void)
{
    if (s_save_timer)
        xTimerReset(s_save_timer, 0);
}

void background_worker_save_now(void)
{
    if (s_save_timer)
        xTimerStop(s_save_timer, 0);
    config_do_save();
}
