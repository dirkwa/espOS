/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include "unity.h"
#include "espos_sensor_math.h"

/* The uncalibrated ADC conversion. Used whenever the chip has no eFuse
 * calibration data, which is where a silent factor-of-something error would
 * show up as a plausible voltage. */
TEST_CASE("adc counts to volts", "[sensors]")
{
    /* 12 dB attenuation, 12-bit: the common case */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, espos_adc_counts_to_volts(0, 3.1f, 4095));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.1f, espos_adc_counts_to_volts(4095, 3.1f, 4095));
    TEST_ASSERT_FLOAT_WITHIN(0.005f, 1.55f, espos_adc_counts_to_volts(2047, 3.1f, 4095));
    /* a smaller attenuation reaches a lower full scale */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.95f, espos_adc_counts_to_volts(4095, 0.95f, 4095));

    /* Out-of-range counts are clamped, not extrapolated: a driver that
     * returned 5000 counts is broken, and reporting 3.8 V from a 3.1 V
     * range would be believed. */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.1f, espos_adc_counts_to_volts(9999, 3.1f, 4095));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, espos_adc_counts_to_volts(-5, 3.1f, 4095));
    /* no divide by zero on a nonsense range */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, espos_adc_counts_to_volts(100, 3.1f, 0));
}

/* Counts over a MEASURED interval. The interval is measured rather than
 * assumed because the first poll after a reconnect or a flash erase is
 * exactly the one that did not run on time. */
TEST_CASE("pulses to hz", "[sensors]")
{
    /* 100 pulses in exactly one second */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 100.0f, espos_pulses_to_hz(100, 1000000));
    /* the same 100 pulses over a poll that ran 10% late: a lower true rate,
     * which is the whole point of measuring the interval */
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 90.909f, espos_pulses_to_hz(100, 1100000));
    /* half a second */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 200.0f, espos_pulses_to_hz(100, 500000));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, espos_pulses_to_hz(0, 1000000));
    /* a zero interval must not divide by zero */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, espos_pulses_to_hz(100, 0));
}

/* The 16-bit PCNT counter restarts at zero when it hits its limit. Getting
 * this wrong produces a plausible RPM, which is the dangerous kind of wrong. */
TEST_CASE("counter delta across the wrap", "[sensors]")
{
    const int limit = 32767;
    TEST_ASSERT_EQUAL_UINT32(0, espos_counter_delta(100, 100, limit));
    TEST_ASSERT_EQUAL_UINT32(50, espos_counter_delta(100, 150, limit));
    TEST_ASSERT_EQUAL_UINT32(100, espos_counter_delta(0, 100, limit));

    /* the wrap: 32700 -> 33 is 100 edges, not a negative count and not
     * 32667 backwards */
    TEST_ASSERT_EQUAL_UINT32(100, espos_counter_delta(32700, 33, limit));
    /* exactly at the limit */
    TEST_ASSERT_EQUAL_UINT32(1, espos_counter_delta(32766, 0, limit));
    TEST_ASSERT_EQUAL_UINT32(0, espos_counter_delta(0, 0, limit));
    /* a nonsense limit returns 0 rather than dividing by it */
    TEST_ASSERT_EQUAL_UINT32(0, espos_counter_delta(5, 10, 0));
}

/* Signal K's propulsion.*.revolutions is per SECOND. Reporting RPM there is
 * the most common unit error on a tachometer and looks plausible either way
 * until someone compares it with the engine's own gauge. */
TEST_CASE("hz to revolutions per second", "[sensors]")
{
    /* a one-pulse-per-revolution sender: Hz IS rev/s */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, espos_hz_to_revolutions(50.0f, 1));
    /* an alternator sender at 6 pulses per revolution */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, espos_hz_to_revolutions(150.0f, 6));
    /* 3000 RPM = 50 rev/s, from a 2-pulse sender at 100 Hz */
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, espos_hz_to_revolutions(100.0f, 2));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, espos_hz_to_revolutions(100.0f, 0));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, espos_hz_to_revolutions(100.0f, -3));
}
