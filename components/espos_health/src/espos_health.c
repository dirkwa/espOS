/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_health — condition table + sink registry. See espos_health.h; the
 * watchdog policy that reads this table is health_policy.c / health_watchdog.c.
 *
 * Everything is a fixed table: the set of conditions a firmware can raise and
 * the set of things that care are both decided at build time, so there is
 * nothing here worth a heap allocation and nothing that can fragment.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_health.h"

static const char *TAG = "espos_health";

#define MAX_CONDITIONS CONFIG_ESPOS_HEALTH_MAX_CONDITIONS
#define MAX_SINKS      CONFIG_ESPOS_HEALTH_MAX_SINKS

typedef struct {
    espos_health_sink_t fn;
    void *arg;
} sink_t;

static struct {
    espos_health_condition_t cond[MAX_CONDITIONS];
    size_t cond_n;
    sink_t sink[MAX_SINKS];
    size_t sink_n;
    SemaphoreHandle_t lock;
} s;

/* Created before app_main by the C runtime, so espos_health_report() works
 * from anywhere without an init call and without the double-checked locking a
 * lazy mutex needs — that pattern relies on an unsynchronised read, which is
 * exactly the kind of thing that works until it does not. */
static void __attribute__((constructor)) health_init(void)
{
    s.lock = xSemaphoreCreateMutex();
}

static bool lock(void)
{
    /* A failure here means the constructor did not run (host builds that link
     * only part of the runtime); report rather than crash. */
    if (!s.lock) return false;
    return xSemaphoreTake(s.lock, pdMS_TO_TICKS(200)) == pdTRUE;
}

static void unlock(void)
{
    xSemaphoreGive(s.lock);
}

const char *espos_health_state_str(espos_health_state_t state)
{
    switch (state) {
    case ESPOS_HEALTH_ALARM: return "alarm";
    case ESPOS_HEALTH_WARN: return "warn";
    default: return "normal";
    }
}

/* Call every sink with the lock released: a sink may report a condition of its
 * own, and espos_sk's sink publishes a delta while holding its own lock. */
static void fan_out(const char *key, espos_health_state_t state, const char *message)
{
    sink_t snapshot[MAX_SINKS];
    size_t n;

    if (!lock()) return;
    n = s.sink_n;
    memcpy(snapshot, s.sink, n * sizeof(snapshot[0]));
    unlock();

    for (size_t i = 0; i < n; i++) {
        snapshot[i].fn(key, state, message, snapshot[i].arg);
    }
}

esp_err_t espos_health_report(const char *key, espos_health_state_t state, const char *message)
{
    return espos_health_report_ex(key, state, message, 0);
}

esp_err_t espos_health_report_ex(const char *key, espos_health_state_t state, const char *message, uint32_t flags)
{
    if (!key || !key[0]) return ESP_ERR_INVALID_ARG;
    if (flags & ~ESPOS_HEALTH_F_REBOOT_ON_ALARM) return ESP_ERR_INVALID_ARG;
    if (!message) message = "";
    if (strlen(key) >= ESPOS_HEALTH_KEY_MAX) return ESP_ERR_INVALID_SIZE;
    if (strlen(message) >= ESPOS_HEALTH_MSG_MAX) return ESP_ERR_INVALID_SIZE;
    if (state != ESPOS_HEALTH_NORMAL && state != ESPOS_HEALTH_WARN &&
        state != ESPOS_HEALTH_ALARM) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!lock()) return ESP_ERR_TIMEOUT;

    espos_health_condition_t *c = NULL;
    for (size_t i = 0; i < s.cond_n; i++) {
        if (strcmp(s.cond[i].key, key) == 0) {
            c = &s.cond[i];
            break;
        }
    }
    if (!c) {
        /* No early-out for a first NORMAL, even though recording "nothing is
         * wrong" looks wasteful: after a reboot a sink's far end may still hold
         * an alert this device raised before it restarted, and the clear that
         * retires it is exactly a first report with NORMAL. */
        if (s.cond_n >= MAX_CONDITIONS) {
            unlock();
            ESP_LOGW(TAG, "no slot for condition '%s' (max %d)", key, MAX_CONDITIONS);
            return ESP_ERR_NO_MEM;
        }
        c = &s.cond[s.cond_n++];
        snprintf(c->key, sizeof(c->key), "%s", key);
        c->state = (espos_health_state_t)-1;  /* forces the first fan-out */
        c->message[0] = '\0';
        c->flags = 0;
    } else if (c->state == state && strcmp(c->message, message) == 0) {
        /* Unchanged for the sinks — stay quiet. The flags still follow the
         * latest report: they are the policy's business, not a sink's. */
        c->flags = flags;
        unlock();
        return ESP_OK;
    }

    c->state = state;
    c->flags = flags;
    snprintf(c->message, sizeof(c->message), "%s", message);
    unlock();

    if (state == ESPOS_HEALTH_NORMAL) {
        ESP_LOGI(TAG, "%s: normal", key);
    } else {
        ESP_LOGW(TAG, "%s: %s (%s)", key, espos_health_state_str(state), message);
    }
    fan_out(key, state, message);
    return ESP_OK;
}

esp_err_t espos_health_add_sink(espos_health_sink_t sink, void *arg)
{
    if (!sink) return ESP_ERR_INVALID_ARG;
    if (!lock()) return ESP_ERR_TIMEOUT;

    for (size_t i = 0; i < s.sink_n; i++) {
        if (s.sink[i].fn == sink && s.sink[i].arg == arg) {
            unlock();
            return ESP_ERR_INVALID_STATE;
        }
    }
    if (s.sink_n >= MAX_SINKS) {
        unlock();
        return ESP_ERR_NO_MEM;
    }
    s.sink[s.sink_n].fn = sink;
    s.sink[s.sink_n].arg = arg;
    s.sink_n++;

    unlock();

    /* Replay with the lock released — same reason as fan_out(). One condition
     * at a time rather than a copy of the whole table: this runs on the
     * caller's task, and a table's worth of conditions is a kilobyte of stack
     * a small task has better uses for. The sink is registered before the
     * replay starts, so a condition reported meanwhile can reach it twice —
     * harmless, because a condition is a level and not an edge. Losing one
     * would not be, which is why the registration comes first. */
    for (size_t i = 0;; i++) {
        espos_health_condition_t c;
        if (!lock()) break;
        if (i >= s.cond_n) {
            unlock();
            break;
        }
        c = s.cond[i];
        unlock();
        sink(c.key, c.state, c.message, arg);
    }
    return ESP_OK;
}

esp_err_t espos_health_remove_sink(espos_health_sink_t sink, void *arg)
{
    if (!lock()) return ESP_ERR_TIMEOUT;
    for (size_t i = 0; i < s.sink_n; i++) {
        if (s.sink[i].fn == sink && s.sink[i].arg == arg) {
            s.sink[i] = s.sink[--s.sink_n];
            unlock();
            return ESP_OK;
        }
    }
    unlock();
    return ESP_ERR_NOT_FOUND;
}

size_t espos_health_snapshot(espos_health_condition_t *out, size_t max)
{
    if (!lock()) return 0;
    size_t n = s.cond_n;
    if (out) {
        size_t copy = n < max ? n : max;
        memcpy(out, s.cond, copy * sizeof(*out));
    }
    unlock();
    return n;
}

espos_health_state_t espos_health_worst(void)
{
    espos_health_state_t worst = ESPOS_HEALTH_NORMAL;
    if (!lock()) return worst;
    for (size_t i = 0; i < s.cond_n; i++) {
        if (s.cond[i].state > worst) worst = s.cond[i].state;
    }
    unlock();
    return worst;
}

bool espos_health_fatal_alarm(espos_health_condition_t *out)
{
    if (!lock()) return false;
    for (size_t i = 0; i < s.cond_n; i++) {
        const espos_health_condition_t *c = &s.cond[i];
        if (c->state == ESPOS_HEALTH_ALARM && (c->flags & ESPOS_HEALTH_F_REBOOT_ON_ALARM)) {
            if (out) *out = *c;
            unlock();
            return true;
        }
    }
    unlock();
    return false;
}

void espos_health_reset(void)
{
    if (!lock()) return;
    s.cond_n = 0;
    s.sink_n = 0;
    unlock();
}
