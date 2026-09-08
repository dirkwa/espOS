/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

#if SOC_PCNT_SUPPORTED
#include "driver/pulse_cnt.h"
#endif

#include "espos_gpio_in.h"
#include "espos_pcnt.h"
#include "espos_sensor_math.h"

static const char *TAG = "espos_pcnt";

/* The 16-bit counter wraps here and restarts at 0; take() allows for one
 * wrap between calls, which at this limit is 32767 edges -- a 1 kHz sensor
 * polled once a second is nowhere near it. */
#define PCNT_LIMIT 32767

struct espos_pcnt {
#if SOC_PCNT_SUPPORTED
    pcnt_unit_handle_t unit;
    int prev;
#else
    /* ISR fallback state. Written in interrupt context, read on a task:
     * volatile, and read once into a local before use. */
    volatile uint32_t edges;
    volatile int64_t last_edge_us;
    int64_t min_gap_us;
    uint32_t prev_edges;
    int gpio;
#endif
    uint64_t total;
    int64_t last_take_us;
};

bool espos_pcnt_supported(void)
{
#if SOC_PCNT_SUPPORTED
    return true;
#else
    return false;
#endif
}

#if !SOC_PCNT_SUPPORTED
/* No PCNT on this chip (the ESP32-C3): count in the GPIO ISR, and do the
 * glitch filter in software. Rejecting an edge closer than min_gap_us to the
 * last one is what the hardware filter does, minus the sub-microsecond
 * resolution. */
static void IRAM_ATTR on_edge(int gpio, bool level, void *arg)
{
    (void)gpio;
    (void)level;
    struct espos_pcnt *h = arg;
    int64_t now = esp_timer_get_time();
    if (h->min_gap_us && now - h->last_edge_us < h->min_gap_us) {
        return;
    }
    h->last_edge_us = now;
    h->edges++;
}
#endif

esp_err_t espos_pcnt_open(const espos_pcnt_cfg_t *cfg, espos_pcnt_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    struct espos_pcnt *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    uint32_t glitch_ns = cfg->max_glitch_ns ? cfg->max_glitch_ns : 1000;
    h->last_take_us = esp_timer_get_time();

#if SOC_PCNT_SUPPORTED
    pcnt_unit_config_t ucfg = { .low_limit = -1, .high_limit = PCNT_LIMIT };
    esp_err_t err = pcnt_new_unit(&ucfg, &h->unit);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    pcnt_glitch_filter_config_t filter = { .max_glitch_ns = glitch_ns };
    /* A filter longer than the peripheral's clock allows is refused; a
     * counter that runs unfiltered is better than one that fails to open. */
    if (pcnt_unit_set_glitch_filter(h->unit, &filter) != ESP_OK) {
        ESP_LOGW(TAG, "glitch filter of %u ns refused; counting unfiltered", (unsigned)glitch_ns);
    }
    pcnt_chan_config_t ccfg = { .edge_gpio_num = cfg->gpio, .level_gpio_num = -1 };
    pcnt_channel_handle_t chan;
    err = pcnt_new_channel(h->unit, &ccfg, &chan);
    if (err != ESP_OK) {
        pcnt_del_unit(h->unit);
        free(h);
        return err;
    }
    pcnt_channel_edge_action_t pos = cfg->falling ? PCNT_CHANNEL_EDGE_ACTION_HOLD : PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    pcnt_channel_edge_action_t neg = cfg->falling ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD;
    pcnt_channel_set_edge_action(chan, pos, neg);
    if (cfg->pull_up) {
        gpio_set_pull_mode(cfg->gpio, GPIO_PULLUP_ONLY);
    }
    pcnt_unit_enable(h->unit);
    pcnt_unit_start(h->unit);
    ESP_LOGI(TAG, "GPIO %d counted by PCNT, %u ns filter", cfg->gpio, (unsigned)glitch_ns);
#else
    h->gpio = cfg->gpio;
    h->min_gap_us = glitch_ns / 1000;
    esp_err_t err = espos_gpio_in_open(cfg->gpio, cfg->pull_up ? ESPOS_GPIO_PULL_UP : ESPOS_GPIO_PULL_NONE, false);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    err = espos_gpio_in_watch(cfg->gpio, cfg->falling ? ESPOS_GPIO_EDGE_FALLING : ESPOS_GPIO_EDGE_RISING, on_edge, h);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    ESP_LOGI(TAG, "GPIO %d counted in software (no PCNT unit), %u ns filter", cfg->gpio, (unsigned)glitch_ns);
#endif
    *out = h;
    return ESP_OK;
}

void espos_pcnt_close(espos_pcnt_handle_t h)
{
    if (!h) {
        return;
    }
#if SOC_PCNT_SUPPORTED
    pcnt_unit_stop(h->unit);
    pcnt_unit_disable(h->unit);
    pcnt_del_unit(h->unit);
#else
    espos_gpio_in_unwatch(h->gpio);
#endif
    free(h);
}

esp_err_t espos_pcnt_take(espos_pcnt_handle_t h, uint32_t *out_count, uint64_t *out_us)
{
    if (!h) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now = esp_timer_get_time();
    uint32_t delta;
#if SOC_PCNT_SUPPORTED
    int count = 0;
    esp_err_t err = pcnt_unit_get_count(h->unit, &count);
    if (err != ESP_OK) {
        return err;
    }
    /* One wrap at PCNT_LIMIT is accounted for; more than one between calls
     * is indistinguishable from none and is a poll that is far too slow. */
    delta = espos_counter_delta(h->prev, count, PCNT_LIMIT);
    h->prev = count;
#else
    /* Read-and-subtract rather than read-and-zero: the ISR only ever adds,
     * so nothing is lost if an edge lands between the two operations. */
    uint32_t edges = h->edges;
    delta = edges - h->prev_edges;
    h->prev_edges = edges;
#endif
    h->total += delta;
    if (out_count) {
        *out_count = delta;
    }
    if (out_us) {
        *out_us = (uint64_t)(now - h->last_take_us);
    }
    h->last_take_us = now;
    return ESP_OK;
}

uint64_t espos_pcnt_total(espos_pcnt_handle_t h) { return h ? h->total : 0; }

void espos_pcnt_set_total(espos_pcnt_handle_t h, uint64_t total)
{
    if (h) {
        h->total = total;
    }
}
