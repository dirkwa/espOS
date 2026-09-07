/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the health watchdog needs from the platform; port_idf.c on chips,
 * port_sim.c on the linux target (no tick, no task watchdog, no memory that
 * survives a restart).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "espos_health_policy.h"

uint32_t espos_health_port_now_ms(void);
uint32_t espos_health_port_uptime_s(void);
int64_t espos_health_port_unix_ms(void);
void espos_health_port_heap(espos_health_heap_t *out);
/** Restart the device. Does not return on a chip; the sim only counts. */
void espos_health_port_restart(void);
void espos_health_port_record_store(const espos_health_reset_record_t *rec);
/** The record the previous boot left, if this boot is its software restart; clears it either way. */
bool espos_health_port_record_take(espos_health_reset_record_t *out);
esp_err_t espos_health_port_tick_start(uint32_t period_ms, void (*cb)(void *arg), void *arg);
/** Task watchdog, for the calling task. ESP_ERR_NOT_SUPPORTED where there is none. */
esp_err_t espos_health_port_twdt_add(void);
esp_err_t espos_health_port_twdt_delete(void);
void espos_health_port_twdt_reset(void);
/** Identity of the calling task, as the policy registry keys on it. */
void *espos_health_port_self(void);
