/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * listener_relay: a GPIO driven by a value the Signal K server streams.
 * Subscribe, copy each value into a queue on the stream task, decide on a
 * task of your own. The queue is the point of the example.
 */
#include <stdbool.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "espos.h"
#include "espos_sk.h"

#if CONFIG_IDF_TARGET_ESP32C6
#define RELAY_GPIO 10 /* relay module input or LED + resistor; any free GPIO on the C6 DevKitC */
#elif CONFIG_IDF_TARGET_ESP32P4
#define RELAY_GPIO 22 /* header pin on the Waveshare panels, clear of the C6 SDIO link (14..19) */
#else
#define RELAY_GPIO 10
#endif
#define WATCH_PATH "environment.outside.illuminance" /* lux, from whichever light sensor the boat has */
#define ON_BELOW   50.0                                 /* dusk: switch on */
#define OFF_ABOVE  100.0                               /* dawn: switch off. The gap is the hysteresis: a cloud at 60 lux changes nothing */
#define STATE_PATH "electrical.switches.deckLight.state"

static const char *TAG = "listener_relay";
static QueueHandle_t s_values;

/* STREAM TASK. The strings in *u live only until this returns, and the task
 * calling us is the one reading the socket: block here and every subscription
 * on the device stalls; call an espos_sk_* that waits on the stream and it
 * deadlocks. So: parse, enqueue without waiting, return. */
static void on_value(const espos_sk_update_t *u, void *arg)
{
    (void)arg;
    if (!u->value_json) return; /* a meta item, not a value */
    char *end;
    double v = strtod(u->value_json, &end);
    if (end == u->value_json) return; /* null, or not a number */
    if (xQueueSend(s_values, &v, 0) != pdTRUE) ESP_LOGW(TAG, "queue full, value dropped"); /* the worker is behind; dropping beats blocking */
}

/* OUR task: free to take its time, log, drive hardware, even call the
 * blocking espos_sk_http_* helpers. */
static void relay_task(void *arg)
{
    (void)arg;
    bool on = false;
    for (;;) {
        double lux;
        if (xQueueReceive(s_values, &lux, portMAX_DELAY) != pdTRUE) continue;
        bool want = on ? !(lux > OFF_ABOVE) : (lux < ON_BELOW);
        if (want == on) continue;
        on = want;
        gpio_set_level(RELAY_GPIO, on);
        espos_sk_publish_bool(STATE_PATH, on); /* so a dashboard sees what the relay did */
        ESP_LOGI(TAG, "%.0f lux -> relay %s", lux, on ? "ON" : "OFF");
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    gpio_config_t out = { .pin_bit_mask = 1ULL << RELAY_GPIO, .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&out));
    s_values = xQueueCreate(8, sizeof(double));
    xTaskCreate(relay_task, "relay", 4096, NULL, 5, NULL);
    /* period_ms is a hint to the server: at most one value per second. The
     * subscription is re-sent after every reconnect; nothing to redo here. */
    int h = espos_sk_subscribe(WATCH_PATH, 1000, on_value, NULL);
    ESP_LOGI(TAG, "subscribed to %s (handle %d); relay on GPIO %d", WATCH_PATH, h, RELAY_GPIO);
}
