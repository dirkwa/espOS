/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * flow_port.h on the linux target. IDF's esp_timer component offers its
 * header there but not esp_timer_get_time(), so the host uses the POSIX
 * monotonic clock directly — the same thing espos_time's and espos_health's
 * port_sim.c do.
 */
#include <time.h>

#include "flow_port.h"

int64_t espos_flow_port_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
