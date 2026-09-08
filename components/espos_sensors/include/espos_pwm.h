/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_pwm — a duty cycle on a pin, via LEDC.
 *
 * Dimming a cabin light, driving an analog gauge, setting a fan speed: all
 * one call with a 0..1 duty. The frequency and resolution trade against each
 * other (the LEDC timer divides one clock, so a high frequency leaves fewer
 * usable bits) and the driver picks the best resolution the requested
 * frequency allows rather than making every caller work it out.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int gpio;
    uint32_t freq_hz;  /* 0 = 5000, which suits an LED and most small loads */
    bool invert;       /* for a low-side driver where full on is a low */
} espos_pwm_cfg_t;

/**
 * Claim an LEDC channel for the pin. Channels are a finite chip resource
 * (6 or 8 depending on the SoC); ESP_ERR_NO_MEM when they are gone.
 */
esp_err_t espos_pwm_open(const espos_pwm_cfg_t *cfg, int *out_channel);

/** Duty 0..1, clamped. */
esp_err_t espos_pwm_set(int channel, float duty);

/** The duty last set. */
float espos_pwm_get(int channel);

esp_err_t espos_pwm_close(int channel);

#ifdef __cplusplus
}
#endif
