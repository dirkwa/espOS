/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The one policy instance a device runs: the periodic tick, the task registry
 * behind espos_health_watch_task()/kick(), and the reset record. Platform
 * calls go through health_port.h so this file is the same on a chip and on
 * the host; the decisions are in health_policy.c.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_health.h"
#include "espos_health_policy.h"
#include "health_port.h"

static const char *TAG = "espos_health";

static struct {
    /* Recursive: a sink reached from the tick's own report may watch, unwatch
     * or read the record, and that is the same task coming back in. */
    SemaphoreHandle_t lock;
    espos_health_policy_t policy;
    bool policy_ready; /* init done (lazily: a task may watch before the tick starts) */
    bool started;
    bool record_checked;
    bool have_record;
    espos_health_reset_record_t record;
    bool twdt_warned;
} s;

static void __attribute__((constructor)) watchdog_init(void)
{
    s.lock = xSemaphoreCreateRecursiveMutex();
}

static bool lock(void)
{
    if (!s.lock) return false;
    return xSemaphoreTakeRecursive(s.lock, pdMS_TO_TICKS(500)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGiveRecursive(s.lock);
}

/* ------------------------------------------------------------------ port */

static uint32_t port_now_ms(void *ctx)
{
    (void)ctx;
    return espos_health_port_now_ms();
}

static uint32_t port_uptime_s(void *ctx)
{
    (void)ctx;
    return espos_health_port_uptime_s();
}

static int64_t port_unix_ms(void *ctx)
{
    (void)ctx;
    return espos_health_port_unix_ms();
}

static void port_heap(void *ctx, espos_health_heap_t *out)
{
    (void)ctx;
    espos_health_port_heap(out);
}

static esp_err_t port_report(void *ctx, const char *key, espos_health_state_t state, const char *message, uint32_t flags)
{
    (void)ctx;
    return espos_health_report_ex(key, state, message, flags);
}

static bool port_fatal_alarm(void *ctx, espos_health_condition_t *out)
{
    (void)ctx;
    return espos_health_fatal_alarm(out);
}

static void port_store_record(void *ctx, const espos_health_reset_record_t *rec)
{
    (void)ctx;
    espos_health_port_record_store(rec);
}

static void port_restart(void *ctx)
{
    (void)ctx;
    ESP_LOGE(TAG, "restarting: %s (%s)", s.policy.fatal.key, s.policy.fatal.message);
    espos_health_port_restart();
}

static const espos_health_policy_port_t PORT = {
    .now_ms = port_now_ms,
    .uptime_s = port_uptime_s,
    .unix_ms = port_unix_ms,
    .heap = port_heap,
    .report = port_report,
    .fatal_alarm = port_fatal_alarm,
    .store_record = port_store_record,
    .restart = port_restart,
};

/* Lock held. */
static void ensure_policy(void)
{
    if (s.policy_ready) return;
    espos_health_policy_cfg_t cfg = {
        .strikes = CONFIG_ESPOS_HEALTH_POLICY_STRIKES,
        .heap_warn_kb = CONFIG_ESPOS_HEALTH_HEAP_WARN_KB,
        .internal_warn_kb = CONFIG_ESPOS_HEALTH_INTERNAL_WARN_KB,
        .internal_alarm_kb = CONFIG_ESPOS_HEALTH_INTERNAL_ALARM_KB,
        .largest_block_alarm_kb = CONFIG_ESPOS_HEALTH_LARGEST_BLOCK_ALARM_KB,
    };
    espos_health_policy_init(&s.policy, &PORT, NULL, &cfg);
    s.policy_ready = true;
}

/* Lock held. Once per boot: whatever the last boot left is read and the stored
 * copy cleared, so a record can never be attributed to a later, unrelated
 * reset (a panic two boots on would otherwise still show it). */
static void take_record(void)
{
    if (s.record_checked) return;
    s.record_checked = true;
    s.have_record = espos_health_port_record_take(&s.record);
    if (s.have_record) {
        ESP_LOGW(TAG, "last reset was the watchdog: %s (%s) after %u s, min internal %u B",
                 s.record.key, s.record.message, (unsigned)s.record.uptime_s, (unsigned)s.record.min_internal);
    }
}

/* ------------------------------------------------------------------ tick */

static void tick(void *arg)
{
    (void)arg;
    if (!lock()) return;
    uint32_t strikes = espos_health_policy_tick(&s.policy);
    if (strikes && !s.policy.restarting) {
        ESP_LOGW(TAG, "strike %u/%u: %s (%s)", (unsigned)strikes, (unsigned)s.policy.cfg.strikes,
                 s.policy.fatal.key, s.policy.fatal.message);
    }
    unlock();
}

esp_err_t espos_health_policy_start(void)
{
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = ESP_OK;
    if (!s.started) {
        take_record();
        ensure_policy();
        err = espos_health_port_tick_start((uint32_t)CONFIG_ESPOS_HEALTH_POLICY_TICK_S * 1000u, tick, NULL);
        if (err == ESP_OK) {
            s.started = true;
            ESP_LOGI(TAG, "watchdog armed: tick %d s, %d strikes; internal RAM alarm below %d KB / block %d KB",
                     CONFIG_ESPOS_HEALTH_POLICY_TICK_S, CONFIG_ESPOS_HEALTH_POLICY_STRIKES,
                     CONFIG_ESPOS_HEALTH_INTERNAL_ALARM_KB, CONFIG_ESPOS_HEALTH_LARGEST_BLOCK_ALARM_KB);
        }
    }
    unlock();
    return err;
}

/* ----------------------------------------------------------- watched tasks */

esp_err_t espos_health_watch_task(const char *name, uint32_t timeout_ms)
{
    if (!name || !name[0]) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_TIMEOUT;
    ensure_policy();
    esp_err_t err = espos_health_policy_watch(&s.policy, espos_health_port_self(), name, timeout_ms);
    if (err == ESP_OK) {
        /* The hard stop. Its absence is not the caller's problem to solve, so
         * it is logged (once) rather than returned: the policy still watches. */
        esp_err_t werr = espos_health_port_twdt_add();
        if (werr != ESP_OK && !s.twdt_warned) {
            s.twdt_warned = true;
            ESP_LOGW(TAG, "task watchdog unavailable (%s): watched tasks are held to the policy tick only",
                     esp_err_to_name(werr));
        }
        ESP_LOGI(TAG, "watching task %s (%u ms)", name, (unsigned)timeout_ms);
    }
    unlock();
    return err;
}

esp_err_t espos_health_unwatch_task(void)
{
    if (!lock()) return ESP_ERR_TIMEOUT;
    esp_err_t err = espos_health_policy_unwatch(&s.policy, espos_health_port_self());
    if (err == ESP_OK) {
        (void)espos_health_port_twdt_delete();
    }
    unlock();
    return err;
}

void espos_health_kick(void)
{
    espos_health_port_twdt_reset();
    /* No lock, by design: this runs in the loop being watched, possibly at
     * display frame rate. The registry is written under the lock in a way a
     * concurrent scan tolerates (health_policy.c). */
    if (s.policy_ready) {
        espos_health_policy_kick(&s.policy, espos_health_port_self());
    }
}

/* ------------------------------------------------------------ reset record */

bool espos_health_last_reset(espos_health_reset_record_t *out)
{
    if (!lock()) return false;
    take_record();
    bool have = s.have_record;
    if (have && out) {
        *out = s.record;
    }
    unlock();
    return have;
}
