/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include "espos_sensor_math.h"

float espos_adc_counts_to_volts(int raw, float full_scale_v, int max_counts)
{
    if (max_counts <= 0) {
        return 0.0f;
    }
    if (raw < 0) {
        raw = 0;
    }
    if (raw > max_counts) {
        raw = max_counts;
    }
    return (float)raw * full_scale_v / (float)max_counts;
}

float espos_pulses_to_hz(uint32_t count, uint64_t interval_us)
{
    if (interval_us == 0) {
        return 0.0f;
    }
    return (float)count * 1e6f / (float)interval_us;
}

uint32_t espos_counter_delta(int prev, int now, int limit)
{
    if (limit <= 0) {
        return 0;
    }
    /* Unsigned arithmetic modulo the limit: `now - prev` is negative exactly
     * when the counter wrapped, and adding the limit before the modulo turns
     * that into the number of edges that actually happened. */
    return (uint32_t)(now - prev + limit) % (uint32_t)limit;
}

float espos_hz_to_revolutions(float hz, int pulses_per_rev)
{
    if (pulses_per_rev <= 0) {
        return 0.0f;
    }
    return hz / (float)pulses_per_rev;
}
