/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_adc — one analog input, calibrated, in volts.
 *
 * The SAR ADC on an ESP32 is not a voltmeter out of the box. Raw counts are
 * several percent off from chip to chip, so the factory burns per-chip
 * coefficients into eFuse and a calibration scheme applies them; WHICH scheme
 * exists is a property of the SoC (curve fitting on the C6/S3/P4, line
 * fitting on the original ESP32), which is why every example that reads an
 * ADC carries the same #if. That belongs in one place, and this is it.
 *
 * The output is volts, never raw counts: counts are meaningless without the
 * attenuation and bit width that produced them, and a calibration a user
 * types into a web UI ("multiply by 1.7") is only stable if what it multiplies
 * is a voltage.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle; one per channel. */
typedef struct espos_adc *espos_adc_handle_t;

/**
 * Attenuation, which sets the input range. The names are the IDF's without
 * the driver header, so a public espOS header stays free of IDF includes.
 * 12 dB reaches roughly 3.1 V and is what a 3.3 V divider wants; the smaller
 * settings clip well below the top of that range.
 */
typedef enum {
    ESPOS_ADC_ATTEN_0DB = 0,  /* ~0.95 V full scale */
    ESPOS_ADC_ATTEN_2_5DB,    /* ~1.25 V */
    ESPOS_ADC_ATTEN_6DB,      /* ~1.75 V */
    ESPOS_ADC_ATTEN_12DB,     /* ~3.1 V  -- the default, and what a divider to 3V3 needs */
} espos_adc_atten_t;

typedef struct {
    int gpio;                  /* the pin. Must be an ADC1 GPIO -- see the note below */
    espos_adc_atten_t atten;   /* ESPOS_ADC_ATTEN_12DB when zero-initialised... see cfg_default */
    uint8_t samples;           /* readings averaged per call; 0 = 16. The SAR ADC is noisy by a few LSB */
} espos_adc_cfg_t;

/**
 * Open one ADC channel.
 *
 * Use an ADC1 pin. ADC2 shares hardware with the radio on several chips and
 * a oneshot read fails while WiFi is up -- which is every moment that matters
 * on a Signal K device, and it fails intermittently rather than at boot.
 *
 * Per-target ADC1 pins, for reference:
 *   ESP32-C6   GPIO 0..6
 *   ESP32-P4   GPIO 16..23 (but 14..19 carry the SDIO link to the co-processor
 *              on the Waveshare panels -- use 20..23 there)
 *   ESP32-C3   GPIO 0..4
 *   ESP32-S3   GPIO 1..10
 *
 * Returns ESP_ERR_NOT_FOUND when the GPIO is not an ADC pin on this chip.
 */
esp_err_t espos_adc_open(const espos_adc_cfg_t *cfg, espos_adc_handle_t *out);
void espos_adc_close(espos_adc_handle_t h);

/**
 * Averaged, calibrated reading in volts.
 *
 * Without eFuse calibration data the result falls back to the nominal full
 * scale for the attenuation, which is accurate to a few percent rather than a
 * few millivolts. espos_adc_calibrated() says which you are getting; it is
 * worth logging once at boot, because a device that is quietly uncalibrated
 * looks exactly like one whose divider is slightly wrong.
 *
 * Blocks for the conversion (microseconds). Safe to call from the flow task.
 */
esp_err_t espos_adc_read_volts(espos_adc_handle_t h, float *out_volts);

/** The averaged raw count, for a caller doing its own conversion. */
esp_err_t espos_adc_read_raw(espos_adc_handle_t h, int *out_raw);

/** Whether eFuse calibration is in use for this channel. */
bool espos_adc_calibrated(espos_adc_handle_t h);

#ifdef __cplusplus
}
#endif
