/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"

#include "espos_gpio_in.h"

static const char *TAG = "espos_gpio";

/* Inversion is a property of the wiring, so it has to be remembered per pin
 * rather than passed to every read. A fixed table keyed by GPIO number: the
 * alternative is a lookup, and there are at most a few dozen pins. */
#define MAX_GPIO 64

static struct {
    bool invert;
    bool is_out;
    bool out_state;
    espos_gpio_isr_cb_t cb;
    void *arg;
} s_pin[MAX_GPIO];

static bool s_isr_service;

static bool valid(int gpio) { return gpio >= 0 && gpio < MAX_GPIO; }

esp_err_t espos_gpio_in_open(int gpio, espos_gpio_pull_t pull, bool invert)
{
    if (!valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << gpio,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = pull == ESPOS_GPIO_PULL_UP ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = pull == ESPOS_GPIO_PULL_DOWN ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_pin[gpio].invert = invert;
    s_pin[gpio].is_out = false;
    return ESP_OK;
}

bool espos_gpio_in_level(int gpio)
{
    if (!valid(gpio)) {
        return false;
    }
    bool level = gpio_get_level(gpio) != 0;
    return s_pin[gpio].invert ? !level : level;
}

/* The shared GPIO ISR dispatcher. Reads the pin rather than trusting the
 * edge that triggered it: with ESPOS_GPIO_EDGE_ANY the level IS the
 * information, and by the time a bouncing contact gets here the edge that
 * caused the interrupt may already be over. */
static void IRAM_ATTR gpio_isr(void *arg)
{
    int gpio = (int)(intptr_t)arg;
    if (!valid(gpio) || !s_pin[gpio].cb) {
        return;
    }
    bool level = gpio_get_level(gpio) != 0;
    if (s_pin[gpio].invert) {
        level = !level;
    }
    s_pin[gpio].cb(gpio, level, s_pin[gpio].arg);
}

esp_err_t espos_gpio_in_watch(int gpio, espos_gpio_edge_t edge, espos_gpio_isr_cb_t cb, void *arg)
{
    if (!valid(gpio) || !cb) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_int_type_t t = GPIO_INTR_ANYEDGE;
    /* The edge names are the electrical ones, so an inverted pin has to
     * swap them: "falling" on an active-low switch is the press, and a
     * caller that asked for the press should not have to know the wiring. */
    bool inv = s_pin[gpio].invert;
    if (edge == ESPOS_GPIO_EDGE_RISING) {
        t = inv ? GPIO_INTR_NEGEDGE : GPIO_INTR_POSEDGE;
    } else if (edge == ESPOS_GPIO_EDGE_FALLING) {
        t = inv ? GPIO_INTR_POSEDGE : GPIO_INTR_NEGEDGE;
    }
    esp_err_t err = gpio_set_intr_type(gpio, t);
    if (err != ESP_OK) {
        return err;
    }
    if (!s_isr_service) {
        err = gpio_install_isr_service(0);
        /* Already installed by something else in the firmware is fine --
         * the service is shared, which is the point of the per-pin API. */
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            return err;
        }
        s_isr_service = true;
    }
    s_pin[gpio].cb = cb;
    s_pin[gpio].arg = arg;
    err = gpio_isr_handler_add(gpio, gpio_isr, (void *)(intptr_t)gpio);
    if (err != ESP_OK) {
        s_pin[gpio].cb = NULL;
        return err;
    }
    ESP_LOGD(TAG, "watching GPIO %d", gpio);
    return ESP_OK;
}

esp_err_t espos_gpio_in_unwatch(int gpio)
{
    if (!valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_isr_handler_remove(gpio);
    gpio_set_intr_type(gpio, GPIO_INTR_DISABLE);
    s_pin[gpio].cb = NULL;
    return ESP_OK;
}

esp_err_t espos_gpio_out_open(int gpio, bool invert, bool initial)
{
    if (!valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    gpio_config_t cfg = { .pin_bit_mask = 1ULL << gpio, .mode = GPIO_MODE_OUTPUT };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    s_pin[gpio].invert = invert;
    s_pin[gpio].is_out = true;
    return espos_gpio_out_set(gpio, initial);
}

esp_err_t espos_gpio_out_set(int gpio, bool on)
{
    if (!valid(gpio)) {
        return ESP_ERR_INVALID_ARG;
    }
    s_pin[gpio].out_state = on;
    return gpio_set_level(gpio, s_pin[gpio].invert ? !on : on);
}

bool espos_gpio_out_get(int gpio) { return valid(gpio) && s_pin[gpio].out_state; }
