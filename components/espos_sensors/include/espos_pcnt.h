/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_pcnt — counting edges, with or without the hardware to do it.
 *
 * A tachometer, a flow meter, a chain counter and an anemometer are all the
 * same problem: how many edges since I last asked. The PCNT peripheral does
 * it in hardware with a glitch filter, and on a chip that has no PCNT unit
 * (the ESP32-C3 has none) the same counting has to happen in an interrupt.
 *
 * Both are behind this one API, because the difference belongs to the chip
 * and not to the sensor. What DOES change is the glitch filter: it is a
 * hardware feature, so on the ISR fallback `max_glitch_ns` is honoured in
 * software by rejecting edges that arrive too soon after the last one --
 * cruder, and it costs an esp_timer read per edge, but a bouncing reed
 * switch otherwise counts three revolutions where there was one.
 *
 * espos_pcnt_supported() says which one you got.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct espos_pcnt *espos_pcnt_handle_t;

typedef struct {
    int gpio;
    uint32_t max_glitch_ns;  /* pulses shorter than this are noise; 0 = 1000 (1 us) */
    bool pull_up;            /* an open-collector sensor pulls the pin low and needs this */
    bool falling;            /* count falling edges instead of rising */
} espos_pcnt_cfg_t;

/** True when the counting is done by the PCNT peripheral rather than an ISR. */
bool espos_pcnt_supported(void);

esp_err_t espos_pcnt_open(const espos_pcnt_cfg_t *cfg, espos_pcnt_handle_t *out);
void espos_pcnt_close(espos_pcnt_handle_t h);

/**
 * Edges since the previous call, and the microseconds those edges span.
 *
 * Returning the interval alongside the count is what makes a frequency
 * honest: dividing by the nominal poll period assumes the poll was on time,
 * and the first reading after a reconnect or a garbage-collecting flash
 * write is exactly when it was not. `us` is measured between successive
 * calls, so `count / (us / 1e6)` is the true average rate over the window.
 *
 * The first call after open() returns whatever accumulated since open and an
 * interval measured from open.
 */
esp_err_t espos_pcnt_take(espos_pcnt_handle_t h, uint32_t *out_count, uint64_t *out_us);

/** Total edges since open(), for a running total that is not rate. */
uint64_t espos_pcnt_total(espos_pcnt_handle_t h);

/** Set the running total (restoring one saved across a reboot). */
void espos_pcnt_set_total(espos_pcnt_handle_t h, uint64_t total);

#ifdef __cplusplus
}
#endif
