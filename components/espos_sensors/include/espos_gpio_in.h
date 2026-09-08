/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_gpio_in — a digital input, read or watched.
 *
 * Two ways to use a pin, and the choice matters more than it looks:
 *
 *   * Sample it (espos_gpio_in_level, on a timer). Costs nothing at a few
 *     Hz, needs no interrupt, and debouncing is a counter. This is what a
 *     switch, a float sensor and a bilge alarm want.
 *   * Watch it (espos_gpio_in_watch, an ISR). The only way to see an edge
 *     shorter than the sampling period -- a pulse, a button you want to feel
 *     instantly, an event that must not be missed.
 *
 * A watch callback runs in interrupt context. What it may do is stated on
 * the function, and the C++ GpioChange node exists so that the answer for
 * graph code is "post to a Mailbox and return".
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_GPIO_PULL_NONE = 0,
    ESPOS_GPIO_PULL_UP,    /* the usual: a switch to ground reads 1 open, 0 closed */
    ESPOS_GPIO_PULL_DOWN,
} espos_gpio_pull_t;

typedef enum {
    ESPOS_GPIO_EDGE_ANY = 0,
    ESPOS_GPIO_EDGE_RISING,
    ESPOS_GPIO_EDGE_FALLING,
} espos_gpio_edge_t;

/**
 * Configure a pin as an input.
 *
 * `invert` reports a closed switch as true when the wiring is
 * active-low, which is nearly every real switch: the internal pull-up holds
 * the pin high and the switch pulls it down. Getting this into the driver
 * rather than into every caller's `!level` is worth the field.
 */
esp_err_t espos_gpio_in_open(int gpio, espos_gpio_pull_t pull, bool invert);

/** Current level, after `invert`. */
bool espos_gpio_in_level(int gpio);

/**
 * Call `cb` from the GPIO ISR on every matching edge.
 *
 * cb runs in interrupt context: no logging, no allocation, no blocking, no
 * FreeRTOS call that is not the FromISR one. Post to a Mailbox (the C++
 * GpioChange node does exactly this) or set a flag, and return.
 *
 * The ISR service is installed on the first watch and shared with anything
 * else in the firmware that uses per-pin GPIO interrupts.
 */
typedef void (*espos_gpio_isr_cb_t)(int gpio, bool level, void *arg);
esp_err_t espos_gpio_in_watch(int gpio, espos_gpio_edge_t edge, espos_gpio_isr_cb_t cb, void *arg);
esp_err_t espos_gpio_in_unwatch(int gpio);

/**
 * Configure a pin as an output and drive it.
 * `invert` flips the sense, for a relay board that switches on a low.
 */
esp_err_t espos_gpio_out_open(int gpio, bool invert, bool initial);
esp_err_t espos_gpio_out_set(int gpio, bool on);
bool espos_gpio_out_get(int gpio);

#ifdef __cplusplus
}
#endif
