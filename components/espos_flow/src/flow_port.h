/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one thing espos_flow needs from the platform: a monotonic microsecond
 * clock. port_idf.c is esp_timer on a chip; port_sim.c is CLOCK_MONOTONIC on
 * the linux target, where esp_timer ships headers but no implementation.
 *
 * Same split as espos_time and espos_health, and for the same reason: the
 * loop's arithmetic is testable on the host only if the clock underneath it
 * is something the host actually has.
 */
#pragma once

#include <stdint.h>

/** Monotonic microseconds since boot (or since the process started, on the
 * host). 64-bit so the loop's own epoch subtraction cannot wrap before the
 * millisecond truncation that the scheduler is designed around. */
int64_t espos_flow_port_now_us(void);
