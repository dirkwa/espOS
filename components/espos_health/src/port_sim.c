/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stand-in for health_port.h. There is no periodic tick (the host test
 * drives its own policy instance with a fake port), no task watchdog and no
 * memory that outlives the process, so espos_health_last_reset() is false and
 * a restart is only counted.
 */
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"

#include "health_port.h"

static const char *TAG = "espos_health";
static int s_restarts;

static uint64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

uint32_t espos_health_port_now_ms(void)
{
    return (uint32_t)mono_ms();
}

uint32_t espos_health_port_uptime_s(void)
{
    return (uint32_t)(mono_ms() / 1000);
}

int64_t espos_health_port_unix_ms(void)
{
    return (int64_t)time(NULL) * 1000;
}

void espos_health_port_heap(espos_health_heap_t *out)
{
    /* The host has no internal/external split; report one healthy pool. */
    uint32_t free_now = esp_get_free_heap_size();
    out->total_free = free_now;
    out->total_min = esp_get_minimum_free_heap_size();
    out->internal_free = free_now;
    out->internal_min = out->total_min;
    out->largest_block = free_now / 2;
}

void espos_health_port_restart(void)
{
    s_restarts++;
    ESP_LOGW(TAG, "sim: restart requested (%d so far)", s_restarts);
}

void espos_health_port_record_store(const espos_health_reset_record_t *rec)
{
    (void)rec;
}

bool espos_health_port_record_take(espos_health_reset_record_t *out)
{
    (void)out;
    return false;
}

esp_err_t espos_health_port_tick_start(uint32_t period_ms, void (*cb)(void *arg), void *arg)
{
    (void)period_ms;
    (void)cb;
    (void)arg;
    return ESP_OK;
}

esp_err_t espos_health_port_twdt_add(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t espos_health_port_twdt_delete(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

void espos_health_port_twdt_reset(void)
{
}

void *espos_health_port_self(void)
{
    return xTaskGetCurrentTaskHandle();
}
