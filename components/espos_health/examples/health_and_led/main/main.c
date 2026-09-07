/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * health_and_led — a status LED driven from the health table, and an
 * application condition the watchdog policy acts on.
 *
 * Off: nothing wrong. Slow blink: a warning. Fast blink: an alarm. The LED
 * never learns which condition: espos_health_worst() is the one value a
 * single LED can show, and it covers espOS's own conditions (netDown,
 * lowMemory) as well as the application's. The application's is a
 * simulated sensor bus that stops answering; held in ALARM with
 * ESPOS_HEALTH_F_REBOOT_ON_ALARM for three policy ticks it restarts the
 * device, and the next boot says why (docs/health.md, the README).
 */
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "espos.h"
#include "espos_health.h"

static const char *TAG = "health_led";

/* A plain LED with a series resistor on a free output pin. GPIO 4 is an
 * output on every supported chip; change it for your board. The on-board
 * LED of most devkits is an addressable RGB LED (a WS2812 on GPIO 8 on the
 * ESP32-C6-DevKitC), which a level on a pin does not drive. */
#define LED_GPIO 4
#define LED_ON   1 /* 0 for an LED wired from 3V3 to the pin */

/* Simulated bus: a frame every second until this much uptime, then silence.
 * 0 = the bus never fails, and the LED shows espOS's own conditions only. */
#define BUS_STOPS_AT_S 60

/* Word-sized, written by the sink and read by the timer: no lock needed. */
static volatile espos_health_state_t s_worst = ESPOS_HEALTH_NORMAL;

/* Sink: called on the task that reported — the policy's esp_timer task, this
 * app's loop, espos_sk's stream task — with no lock held, so it does the
 * minimum. The GPIO work is the timer's, at its own pace. */
static void led_sink(const char *key, espos_health_state_t state, const char *message, void *arg)
{
    (void)arg;
    s_worst = espos_health_worst();
    ESP_LOGI(TAG, "%s → %s (%s); LED shows %s", key, espos_health_state_str(state), message ? message : "",
             espos_health_state_str(s_worst));
}

/* Every 100 ms: off, 1 Hz or 5 Hz. A fixed tick that reads the pattern beats
 * restarting the timer from the sink — nothing to keep in step. */
static void led_tick(void *arg)
{
    (void)arg;
    static uint32_t n;
    n++;
    bool on = false;
    if (s_worst == ESPOS_HEALTH_WARN) {
        on = (n / 5) & 1; /* 500 ms on, 500 ms off */
    } else if (s_worst == ESPOS_HEALTH_ALARM) {
        on = n & 1; /* 100 ms on, 100 ms off */
    }
    gpio_set_level(LED_GPIO, on ? LED_ON : !LED_ON);
}

void app_main(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << LED_GPIO, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&io));
    const esp_timer_create_args_t tick = { .callback = led_tick, .name = "led" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&tick, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, 100 * 1000));

    /* Before espos_start(): registering replays every condition recorded so
     * far, and from here on the LED shows whatever espOS reports as well —
     * a netDown when the router goes away, lowMemory from the policy tick. */
    ESP_ERROR_CHECK(espos_health_add_sink(led_sink, NULL));
    ESP_ERROR_CHECK(espos_start(NULL));

    /* The application's own condition: a bus that must answer. Level-
     * triggered — re-reported on every poll, sinks fire only on a change.
     * The message is fixed per state: sinks fan out on a changed message too,
     * and a seconds counter in the text would re-notify the server every
     * second. The flag makes ALARM fatal, because a restart IS the recovery
     * for a wedged bus — never for a network that is merely away. */
    int64_t last_frame_ms = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t now_ms = esp_timer_get_time() / 1000;
        if (BUS_STOPS_AT_S == 0 || now_ms < (int64_t)BUS_STOPS_AT_S * 1000) {
            last_frame_ms = now_ms; /* a frame arrived */
        }
        int64_t silent_s = (now_ms - last_frame_ms) / 1000;
        if (silent_s >= 30) {
            espos_health_report_ex("sensorBus", ESPOS_HEALTH_ALARM, "no frames for 30 s", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
        } else if (silent_s >= 5) {
            espos_health_report_ex("sensorBus", ESPOS_HEALTH_WARN, "no frames for 5 s", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
        } else {
            espos_health_report_ex("sensorBus", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
        }
    }
}
