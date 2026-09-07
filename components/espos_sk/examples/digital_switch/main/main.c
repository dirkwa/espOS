/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * digital_switch: a relay (or LED) on a GPIO whose state is a Signal K switch
 * path, toggled by a debounced button. Published on every change and every
 * 10 s, so a dashboard that missed the change still shows the truth.
 */
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "espos.h"
#include "espos_sk.h"

#if CONFIG_IDF_TARGET_ESP32C6
#define SWITCH_GPIO 10 /* relay module input or LED + resistor; any free GPIO on the C6 DevKitC */
#define BUTTON_GPIO 9  /* the BOOT button every C6 devkit has (to GND; a strapping pin, harmless as an input after boot) */
#elif CONFIG_IDF_TARGET_ESP32P4
#define SWITCH_GPIO 22 /* header pins on the Waveshare panels, clear of the C6 SDIO link (14..19) */
#define BUTTON_GPIO 23 /* a button from here to GND; the internal pull-up does the rest */
#else
#define SWITCH_GPIO 10
#define BUTTON_GPIO 9
#endif
#define DEBOUNCE_TICK_MS 10 /* how often the button is sampled */
#define DEBOUNCE_TICKS   4    /* 40 ms of the same level before it counts; contacts bounce for 1..20 ms */
#define REPUBLISH_MS     10000
#define SK_PATH          "electrical.switches.bilge.state" /* spec: the <id> segment is yours, "state" is a boolean */

static const char *TAG = "digital_switch";
static bool s_on; /* one word: written on the esp_timer task, read by the loop, no lock */

static void set_switch(bool on)
{
    s_on = on;
    gpio_set_level(SWITCH_GPIO, on);
    espos_sk_publish_bool(SK_PATH, on); /* thread-safe and never blocks, so fine from a timer callback */
    ESP_LOGI(TAG, "%s", on ? "ON" : "OFF");
}

/* Every 10 ms on the esp_timer task. A GPIO interrupt would fire on every
 * bounce and need a timer anyway; sampling needs no interrupt at all and
 * costs nothing at this rate. Counts how long the level has been steady; a
 * press (low, because of the pull-up) that lasts DEBOUNCE_TICKS toggles once. */
static void debounce_tick(void *arg)
{
    (void)arg;
    static int last = 1, steady;
    int level = gpio_get_level(BUTTON_GPIO);
    if (level != last) {
        last = level;
        steady = 0;
    } else if (steady < DEBOUNCE_TICKS && ++steady == DEBOUNCE_TICKS && level == 0) {
        set_switch(!s_on);
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    gpio_config_t out = { .pin_bit_mask = 1ULL << SWITCH_GPIO, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
    gpio_config_t in = { .pin_bit_mask = 1ULL << BUTTON_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&in));
    set_switch(false); /* a known state on boot, and the first publish */
    const esp_timer_create_args_t targs = { .callback = debounce_tick, .name = "debounce" };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&targs, &timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(timer, DEBOUNCE_TICK_MS * 1000));
    /* Inbound control -- a PUT from the server to this path -- is not handled
     * here: espos_sk has no PUT-request handler yet; it lands with the
     * data-flow tranche. Until then the button and this loop are the only
     * writers, and a dashboard's switch widget shows the state but cannot
     * flip it. Nothing here pretends otherwise. */
    for (;; vTaskDelay(pdMS_TO_TICKS(REPUBLISH_MS))) {
        espos_sk_publish_bool(SK_PATH, s_on); /* a fresh timestamp for a value that rarely changes */
    }
}
