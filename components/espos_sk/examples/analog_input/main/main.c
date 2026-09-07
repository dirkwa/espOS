/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * analog_input: one ADC pin, calibrated, averaged, scaled by two settings the
 * web UI edits live, published as a Signal K value. The shape every "sensor
 * on a wire" firmware has; swap the path and the calibration.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"

/* The sensor pin: any ADC1-capable GPIO. ADC2 shares hardware with the radio
 * on several chips and a oneshot read then fails while WiFi is up. */
#if CONFIG_IDF_TARGET_ESP32C6
#define ADC_GPIO 4 /* ADC1_CH4; GPIO 0..6 are ADC1 on the C6 DevKitC */
#elif CONFIG_IDF_TARGET_ESP32P4
#define ADC_GPIO 20 /* ADC1_CH4; 16..19 are ADC1 too but carry the C6 SDIO link on the Waveshare panels */
#else
#define ADC_GPIO 4 /* ADC1 on the C3 and S3; the original ESP32 wants 32..39 */
#endif
#define ADC_SAMPLES 16                           /* averaged per reading: the SAR ADC is noisy by a few LSB */
#define SK_PATH     "environment.inside.illuminance" /* a spec path: the server owns its meta */

static const char *TAG = "analog_input";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali; /* NULL when the chip has no calibration data */
static adc_channel_t s_chan;
/* Word-sized: written on the HTTP handler's task, read by the loop, no lock. */
static float s_multiplier = 1.0f, s_offset = 0.0f;
static int32_t s_period_ms = 1000;

/* Reads the settings. Also the change callback, so a PUT from the web UI is
 * in effect for the next reading without a restart. Runs on the writer's
 * task (an HTTP handler) with the store unlocked, so reading back is fine. */
static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns;
    (void)key;
    (void)arg;
    espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_MULTIPLIER, &s_multiplier);
    espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_OFFSET, &s_offset);
    espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_PERIOD_MS, &s_period_ms);
}

static void adc_init(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &unit, &s_chan));
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    /* 12 dB attenuation reaches ~3.1 V full scale; the smaller settings clip
     * a 3.3 V divider well below its top. */
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &ccfg));
    /* Raw counts are off by several percent from chip to chip. The factory
     * burns per-chip coefficients into eFuse and a scheme applies them; which
     * scheme exists is a property of the SoC, hence the #if. */
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cal = { .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_curve_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cal = { .unit_id = unit, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    if (adc_cali_create_scheme_line_fitting(&cal, &s_cali) != ESP_OK) s_cali = NULL;
#endif
    ESP_LOGI(TAG, "GPIO %d = ADC%d channel %d, calibration %s", ADC_GPIO, unit + 1, s_chan, s_cali ? "on" : "unavailable");
}

/* Averaged, calibrated reading in volts. */
static float read_volts(void)
{
    int sum = 0, raw = 0, mv = 0;
    for (int i = 0; i < ADC_SAMPLES; i++) {
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan, &raw));
        sum += raw;
    }
    raw = sum / ADC_SAMPLES;
    if (s_cali && adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) return mv / 1000.0f;
    return raw * 3.1f / 4095.0f; /* no calibration data: nominal 12 dB full scale over 12 bits */
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    adc_init();
    /* For a custom path instead of SK_PATH: nobody but this device knows what
     * it means, so declare its meta once. It is reconciled on every connect
     * and the server's copy wins if a user edited it there. Never for a spec
     * path such as SK_PATH -- those belong to the server.
     *
     *   espos_sk_declare_meta("sensors.analog.0.voltage",
     *       "{\"units\":\"V\",\"description\":\"ADC input after the divider\"}", s_period_ms);
     *
     * The tank-level variant is a spec path again: "tanks.freshWater.0.currentLevel"
     * is a ratio 0..1, so choose multiplier and offset such that empty reads 0
     * and full reads 1. */
    for (;; vTaskDelay(pdMS_TO_TICKS(s_period_ms))) {
        float volts = read_volts();
        float value = volts * s_multiplier + s_offset; /* the calibration a user types into the web UI */
        espos_sk_publish_number(SK_PATH, value);
        ESP_LOGI(TAG, "%.3f V -> %s = %.3f", volts, SK_PATH, value); /* the volts are what you calibrate against */
    }
}
