/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>

#include "driver/ledc.h"
#include "esp_log.h"

#include "espos_pwm.h"

static const char *TAG = "espos_pwm";

#define SPEED_MODE LEDC_LOW_SPEED_MODE

static struct {
    bool used;
    bool invert;
    float duty;
    ledc_timer_bit_t bits;
} s_ch[LEDC_CHANNEL_MAX];

/* The LEDC timer divides one source clock, so resolution and frequency trade
 * against each other: bits are usable only while freq * 2^bits stays under
 * the clock. Walking down from 13 bits picks the finest control the
 * requested frequency actually allows, instead of failing or silently
 * quantising to something coarse. */
static ledc_timer_bit_t best_bits(uint32_t freq_hz)
{
    for (int b = LEDC_TIMER_13_BIT; b >= LEDC_TIMER_4_BIT; b--) {
        if ((uint64_t)freq_hz << b <= 80000000ULL) {
            return (ledc_timer_bit_t)b;
        }
    }
    return LEDC_TIMER_4_BIT;
}

esp_err_t espos_pwm_open(const espos_pwm_cfg_t *cfg, int *out_channel)
{
    if (!cfg || !out_channel) {
        return ESP_ERR_INVALID_ARG;
    }
    int ch = -1;
    for (int i = 0; i < LEDC_CHANNEL_MAX; i++) {
        if (!s_ch[i].used) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        ESP_LOGE(TAG, "no free LEDC channel");
        return ESP_ERR_NO_MEM;
    }
    uint32_t freq = cfg->freq_hz ? cfg->freq_hz : 5000;
    ledc_timer_bit_t bits = best_bits(freq);

    /* One timer shared by every channel opened here: the common case is
     * several outputs at the same frequency (a switch panel's backlights),
     * and a timer each would run the chip out of them at four. */
    ledc_timer_config_t tcfg = {
        .speed_mode = SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = bits,
        .freq_hz = freq,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    esp_err_t err = ledc_timer_config(&tcfg);
    if (err != ESP_OK) {
        return err;
    }
    ledc_channel_config_t ccfg = {
        .gpio_num = cfg->gpio,
        .speed_mode = SPEED_MODE,
        .channel = (ledc_channel_t)ch,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&ccfg);
    if (err != ESP_OK) {
        return err;
    }
    s_ch[ch].used = true;
    s_ch[ch].invert = cfg->invert;
    s_ch[ch].bits = bits;
    s_ch[ch].duty = 0.0f;
    *out_channel = ch;
    ESP_LOGI(TAG, "GPIO %d = LEDC channel %d, %u Hz, %d bits", cfg->gpio, ch, (unsigned)freq, (int)bits);
    return espos_pwm_set(ch, 0.0f);
}

esp_err_t espos_pwm_set(int channel, float duty)
{
    if (channel < 0 || channel >= LEDC_CHANNEL_MAX || !s_ch[channel].used) {
        return ESP_ERR_INVALID_ARG;
    }
    if (duty < 0.0f) {
        duty = 0.0f;
    }
    if (duty > 1.0f) {
        duty = 1.0f;
    }
    s_ch[channel].duty = duty;
    float d = s_ch[channel].invert ? 1.0f - duty : duty;
    uint32_t max = (1u << s_ch[channel].bits) - 1;
    esp_err_t err = ledc_set_duty(SPEED_MODE, (ledc_channel_t)channel, (uint32_t)(d * max + 0.5f));
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(SPEED_MODE, (ledc_channel_t)channel);
}

float espos_pwm_get(int channel)
{
    if (channel < 0 || channel >= LEDC_CHANNEL_MAX) {
        return 0.0f;
    }
    return s_ch[channel].duty;
}

esp_err_t espos_pwm_close(int channel)
{
    if (channel < 0 || channel >= LEDC_CHANNEL_MAX || !s_ch[channel].used) {
        return ESP_ERR_INVALID_ARG;
    }
    ledc_stop(SPEED_MODE, (ledc_channel_t)channel, 0);
    s_ch[channel].used = false;
    return ESP_OK;
}
