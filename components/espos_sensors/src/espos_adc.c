/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdlib.h>
#include <string.h>

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

#include "espos_adc.h"
#include "espos_sensor_math.h"

static const char *TAG = "espos_adc";

#define DEFAULT_SAMPLES 16

struct espos_adc {
    adc_oneshot_unit_handle_t unit;
    adc_cali_handle_t cali; /* NULL when the chip has no calibration data */
    adc_channel_t chan;
    adc_atten_t atten;
    uint8_t samples;
};

static adc_atten_t to_idf_atten(espos_adc_atten_t a)
{
    switch (a) {
    case ESPOS_ADC_ATTEN_0DB:
        return ADC_ATTEN_DB_0;
    case ESPOS_ADC_ATTEN_2_5DB:
        return ADC_ATTEN_DB_2_5;
    case ESPOS_ADC_ATTEN_6DB:
        return ADC_ATTEN_DB_6;
    default:
        return ADC_ATTEN_DB_12;
    }
}

/* Nominal full scale per attenuation, used only when the chip has no eFuse
 * calibration. Accurate to a few percent, which is the honest accuracy of an
 * uncalibrated reading anyway. */
static float nominal_full_scale(adc_atten_t a)
{
    switch (a) {
    case ADC_ATTEN_DB_0:
        return 0.95f;
    case ADC_ATTEN_DB_2_5:
        return 1.25f;
    case ADC_ATTEN_DB_6:
        return 1.75f;
    default:
        return 3.1f;
    }
}

esp_err_t espos_adc_open(const espos_adc_cfg_t *cfg, espos_adc_handle_t *out)
{
    if (!cfg || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;

    adc_unit_t unit;
    adc_channel_t chan;
    esp_err_t err = adc_oneshot_io_to_channel(cfg->gpio, &unit, &chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GPIO %d is not an ADC pin on this chip", cfg->gpio);
        return ESP_ERR_NOT_FOUND;
    }
    /* ADC2 is shared with the radio: a oneshot read returns
     * ESP_ERR_TIMEOUT while WiFi is transmitting. Warn rather than refuse --
     * a device with no radio in use is entitled to the pin. */
    if (unit != ADC_UNIT_1) {
        ESP_LOGW(TAG, "GPIO %d is on ADC%d; reads can fail while the radio is up", cfg->gpio, unit + 1);
    }

    struct espos_adc *h = calloc(1, sizeof(*h));
    if (!h) {
        return ESP_ERR_NO_MEM;
    }
    h->chan = chan;
    h->atten = to_idf_atten(cfg->atten);
    h->samples = cfg->samples ? cfg->samples : DEFAULT_SAMPLES;

    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    err = adc_oneshot_new_unit(&ucfg, &h->unit);
    if (err != ESP_OK) {
        free(h);
        return err;
    }
    adc_oneshot_chan_cfg_t ccfg = { .atten = h->atten, .bitwidth = ADC_BITWIDTH_DEFAULT };
    err = adc_oneshot_config_channel(h->unit, chan, &ccfg);
    if (err != ESP_OK) {
        adc_oneshot_del_unit(h->unit);
        free(h);
        return err;
    }

    /* Whichever scheme the SoC has. Failing to create one is not an error:
     * the reading falls back to the nominal full scale. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = {
        .unit_id = unit, .chan = chan, .atten = h->atten, .bitwidth = ADC_BITWIDTH_DEFAULT
    };
    if (adc_cali_create_scheme_curve_fitting(&cal, &h->cali) != ESP_OK) {
        h->cali = NULL;
    }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = { .unit_id = unit, .atten = h->atten, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_line_fitting(&cal, &h->cali) != ESP_OK) {
        h->cali = NULL;
    }
#endif
    ESP_LOGI(TAG, "GPIO %d = ADC%d channel %d, calibration %s", cfg->gpio, unit + 1, chan, h->cali ? "on" : "unavailable");
    *out = h;
    return ESP_OK;
}

void espos_adc_close(espos_adc_handle_t h)
{
    if (!h) {
        return;
    }
    if (h->cali) {
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        adc_cali_delete_scheme_curve_fitting(h->cali);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        adc_cali_delete_scheme_line_fitting(h->cali);
#endif
    }
    adc_oneshot_del_unit(h->unit);
    free(h);
}

esp_err_t espos_adc_read_raw(espos_adc_handle_t h, int *out_raw)
{
    if (!h || !out_raw) {
        return ESP_ERR_INVALID_ARG;
    }
    int sum = 0;
    for (uint8_t i = 0; i < h->samples; i++) {
        int raw = 0;
        esp_err_t err = adc_oneshot_read(h->unit, h->chan, &raw);
        if (err != ESP_OK) {
            return err;
        }
        sum += raw;
    }
    *out_raw = sum / h->samples;
    return ESP_OK;
}

esp_err_t espos_adc_read_volts(espos_adc_handle_t h, float *out_volts)
{
    if (!h || !out_volts) {
        return ESP_ERR_INVALID_ARG;
    }
    int raw = 0;
    esp_err_t err = espos_adc_read_raw(h, &raw);
    if (err != ESP_OK) {
        return err;
    }
    int mv = 0;
    if (h->cali && adc_cali_raw_to_voltage(h->cali, raw, &mv) == ESP_OK) {
        *out_volts = mv / 1000.0f;
        return ESP_OK;
    }
    /* 12-bit default width on every chip espOS targets. The arithmetic is
     * in espos_sensor_math.c because a wrong answer here is silent. */
    *out_volts = espos_adc_counts_to_volts(raw, nominal_full_scale(h->atten), 4095);
    return ESP_OK;
}

bool espos_adc_calibrated(espos_adc_handle_t h) { return h && h->cali; }
