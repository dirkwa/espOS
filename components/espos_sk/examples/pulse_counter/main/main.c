/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * pulse_counter: pulses on a GPIO become revolutions per second on a Signal K path, plus
 * a running total that survives a reboot. RPM, shaft speed, chain counter, flow meter: one loop.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"

/* The pulse input; an open-collector sensor pulls it low, the internal pull-up does the rest. */
#if CONFIG_IDF_TARGET_ESP32C6
#define PULSE_GPIO 5 /* any free GPIO on the C6 DevKitC */
#elif CONFIG_IDF_TARGET_ESP32P4
#define PULSE_GPIO 21 /* header pin on the Waveshare panels, clear of the C6 SDIO link (14..19) */
#else
#define PULSE_GPIO 4 /* C3 DevKitM and most other boards */
#endif
#define PCNT_LIMIT       32767                      /* the 16-bit counter restarts at 0 here; take_pulses() allows for it */
#define PERSIST_EVERY_MS 60000                /* NVS is flash with ~100k erase cycles per sector; see the README */
#define SK_PATH          "propulsion.main.revolutions" /* spec path, unit Hz: revolutions per SECOND, not per minute */

static const char *TAG = "pulse_counter";
/* Word-sized: written on the HTTP handler's task, read by the loop, no lock. */
static int32_t s_ppr = 1, s_period_ms = 1000, s_total;

static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns;
    (void)arg;
    espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_PULSES_PER_REV, &s_ppr);
    espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_PERIOD_MS, &s_period_ms);
    /* The total is ours to write, but a user zeroing it in the web UI must win too (our own save lands here as well). */
    if (!key || strcmp(key, ESPOS_CFG_APP_TOTAL_PULSES) == 0) espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_TOTAL_PULSES, &s_total);
}

#if SOC_PCNT_SUPPORTED
/* The pulse counter peripheral counts edges and drops glitches in hardware; the CPU reads a number now and then. */
static pcnt_unit_handle_t s_unit;
static void counter_init(void)
{
    pcnt_unit_config_t ucfg = { .low_limit = -1, .high_limit = PCNT_LIMIT };
    ESP_ERROR_CHECK(pcnt_new_unit(&ucfg, &s_unit));
    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = 1000 }; /* contact bounce and pickup shorter than 1 µs vanish */
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_unit, &filter));
    pcnt_chan_config_t ccfg = { .edge_gpio_num = PULSE_GPIO, .level_gpio_num = -1 };
    pcnt_channel_handle_t chan;
    ESP_ERROR_CHECK(pcnt_new_channel(s_unit, &ccfg, &chan));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(gpio_set_pull_mode(PULSE_GPIO, GPIO_PULLUP_ONLY));
    ESP_ERROR_CHECK(pcnt_unit_enable(s_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_unit));
}
/* Pulses since the previous call; one wrap at PCNT_LIMIT is accounted for. */
static uint32_t take_pulses(void)
{
    static int prev, now;
    ESP_ERROR_CHECK(pcnt_unit_get_count(s_unit, &now));
    uint32_t delta = (uint32_t)(now - prev + PCNT_LIMIT) % PCNT_LIMIT;
    prev = now;
    return delta;
}
#else
/* No PCNT unit on this chip (ESP32-C3): count edges in an interrupt. No glitch filter this way. */
static volatile uint32_t s_edges;
static void IRAM_ATTR on_edge(void *arg) { s_edges++; }
static void counter_init(void)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << PULSE_GPIO, .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE, .intr_type = GPIO_INTR_POSEDGE };
    ESP_ERROR_CHECK(gpio_config(&io));
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PULSE_GPIO, on_edge, NULL));
}
static uint32_t take_pulses(void)
{
    static uint32_t prev;
    uint32_t now = s_edges, delta = now - prev; /* one word, one read: atomic on every target */
    prev = now;
    return delta;
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    counter_init();
    ESP_LOGI(TAG, "counting on GPIO %d, %ld pulses per revolution, total so far %ld", PULSE_GPIO, (long)s_ppr, (long)s_total);
    int32_t saved = s_total;
    int64_t last_us = esp_timer_get_time(), saved_us = last_us;
    for (;; vTaskDelay(pdMS_TO_TICKS(s_period_ms))) {
        uint32_t pulses = take_pulses();
        int64_t now_us = esp_timer_get_time(); /* measured, not assumed: the loop period is only roughly s_period_ms */
        double hz = (double)pulses * 1e6 / (double)(now_us - last_us) / (double)s_ppr;
        last_us = now_us;
        s_total += (int32_t)pulses;
        espos_sk_publish_number(SK_PATH, hz);
        /* The total is a setting so the web UI shows it and a user can zero it. Saved at most
         * once a minute and only when it moved: every espos_config_set_* is a flash write. */
        if (s_total != saved && now_us - saved_us >= (int64_t)PERSIST_EVERY_MS * 1000) {
            espos_config_set_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_TOTAL_PULSES, s_total);
            saved = s_total;
            saved_us = now_us;
        }
    }
}
