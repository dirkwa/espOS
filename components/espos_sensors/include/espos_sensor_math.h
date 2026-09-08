/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sensor_math — the arithmetic of the sensor drivers, with no hardware
 * under it.
 *
 * These four functions are the only places in espos_sensors where a wrong
 * answer is silent: a counter that mis-handles its wrap reports a plausible
 * RPM, and an uncalibrated ADC conversion that is off by a factor reports a
 * plausible voltage. Everything else in the component either works or
 * returns an error.
 *
 * So they live here, pure and host-tested, rather than inline in the drivers
 * (see test/host/espos_sensors_test). The drivers call them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Raw ADC counts to volts, with no eFuse calibration available.
 *
 * `full_scale_v` is the nominal full-scale voltage of the attenuation in use
 * and `max_counts` the top of the range (4095 at 12 bits). Accurate to a few
 * percent, which is the honest accuracy of an uncalibrated SAR ADC.
 */
float espos_adc_counts_to_volts(int raw, float full_scale_v, int max_counts);

/**
 * Edges counted over a measured interval, as a frequency in Hz.
 *
 * The interval is measured rather than assumed: dividing by the nominal poll
 * period assumes the poll ran on time, and the first poll after a reconnect
 * or a flash erase is exactly when it did not. A zero interval returns 0
 * rather than dividing by it.
 */
float espos_pulses_to_hz(uint32_t count, uint64_t interval_us);

/**
 * The number of edges between two readings of a counter that wraps at
 * `limit` and restarts at zero.
 *
 * One wrap between calls is accounted for. More than one is
 * indistinguishable from none -- the counter is 16-bit and carries no
 * generation -- so a poll must be fast enough that fewer than `limit` edges
 * arrive between two of them.
 */
uint32_t espos_counter_delta(int prev, int now, int limit);

/**
 * Revolutions per second from a pulse frequency and the pulses per
 * revolution of the sender.
 *
 * Signal K's propulsion.*.revolutions is per SECOND, not per minute: the
 * single most common unit error on a tachometer, and one that looks
 * plausible either way until someone compares it with the engine's own gauge.
 */
float espos_hz_to_revolutions(float hz, int pulses_per_rev);

#ifdef __cplusplus
}
#endif
