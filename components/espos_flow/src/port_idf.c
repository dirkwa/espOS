/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * flow_port.h on a chip: esp_timer's 64-bit microsecond counter, which is
 * what every other espOS component measures elapsed time with.
 */
#include "esp_timer.h"

#include "flow_port.h"

int64_t espos_flow_port_now_us(void)
{
    return esp_timer_get_time();
}
