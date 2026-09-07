/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * The watchdog policy, pure C. See espos_health_policy.h; the device glue is
 * health_watchdog.c, the host test drives this directly with a fake port.
 */
#include <stdio.h>
#include <string.h>

#include "espos_health_policy.h"

/* Magic of a reset record; shared with the ports through the header's type
 * only, so it lives here where the record is written. */
#define RECORD_MAGIC 0x6865616Cu /* "heal" */

void espos_health_policy_init(espos_health_policy_t *p, const espos_health_policy_port_t *port, void *ctx,
                              const espos_health_policy_cfg_t *cfg)
{
    memset(p, 0, sizeof(*p));
    p->port = port;
    p->ctx = ctx;
    p->cfg = *cfg;
    if (p->cfg.strikes == 0) {
        p->cfg.strikes = 1;
    }
}

espos_health_state_t espos_health_policy_memory(const espos_health_policy_cfg_t *cfg, const espos_health_heap_t *h,
                                                char *message, size_t message_size, uint32_t *flags)
{
    uint32_t f = 0;
    espos_health_state_t st = ESPOS_HEALTH_NORMAL;
    const char *m = "";
    char buf[ESPOS_HEALTH_MSG_MAX];

    /* Alarms first, then warnings: internal RAM is judged on its own because on
     * a PSRAM board the total can read tens of megabytes while the internal
     * pool — the one the radio, DMA and the task stacks come from — is gone. */
    if (cfg->internal_alarm_kb && h->internal_free < cfg->internal_alarm_kb * 1024u) {
        st = ESPOS_HEALTH_ALARM;
        f = ESPOS_HEALTH_F_REBOOT_ON_ALARM;
        snprintf(buf, sizeof(buf), "internal RAM exhausted: %u B free (alarm below %u KB)",
                 (unsigned)h->internal_free, (unsigned)cfg->internal_alarm_kb);
        m = buf;
    } else if (cfg->largest_block_alarm_kb && h->largest_block < cfg->largest_block_alarm_kb * 1024u) {
        st = ESPOS_HEALTH_ALARM;
        f = ESPOS_HEALTH_F_REBOOT_ON_ALARM;
        snprintf(buf, sizeof(buf), "internal RAM fragmented: largest block %u B (alarm below %u KB)",
                 (unsigned)h->largest_block, (unsigned)cfg->largest_block_alarm_kb);
        m = buf;
    } else if (cfg->internal_warn_kb && h->internal_free < cfg->internal_warn_kb * 1024u) {
        st = ESPOS_HEALTH_WARN;
        snprintf(buf, sizeof(buf), "internal RAM low: %u B free (warn below %u KB)",
                 (unsigned)h->internal_free, (unsigned)cfg->internal_warn_kb);
        m = buf;
    } else if (cfg->heap_warn_kb && h->total_free < cfg->heap_warn_kb * 1024u) {
        st = ESPOS_HEALTH_WARN;
        snprintf(buf, sizeof(buf), "heap low: %u B free (warn below %u KB)",
                 (unsigned)h->total_free, (unsigned)cfg->heap_warn_kb);
        m = buf;
    }
    if (message && message_size) {
        snprintf(message, message_size, "%s", m);
    }
    if (flags) {
        *flags = f;
    }
    return st;
}

static void check_tasks(espos_health_policy_t *p, uint32_t now)
{
    const espos_health_watched_t *stalled = NULL;
    uint32_t worst_age = 0;
    for (size_t i = 0; i < ESPOS_HEALTH_WATCHED_MAX; i++) {
        const espos_health_watched_t *w = &p->watched[i];
        if (!w->task) {
            continue;
        }
        uint32_t age = now - w->last_kick_ms;
        if (age > w->timeout_ms && age >= worst_age) {
            stalled = w;
            worst_age = age;
        }
    }
    if (stalled) {
        char m[ESPOS_HEALTH_MSG_MAX];
        snprintf(m, sizeof(m), "%s silent for %u s (limit %u s)", stalled->name, (unsigned)(worst_age / 1000),
                 (unsigned)(stalled->timeout_ms / 1000));
        p->port->report(p->ctx, "taskStalled", ESPOS_HEALTH_ALARM, m, ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    } else {
        p->port->report(p->ctx, "taskStalled", ESPOS_HEALTH_NORMAL, "", ESPOS_HEALTH_F_REBOOT_ON_ALARM);
    }
}

uint32_t espos_health_policy_tick(espos_health_policy_t *p)
{
    if (p->restarting) {
        return p->strikes;
    }
    p->ticks++;
    uint32_t now = p->port->now_ms(p->ctx);

    espos_health_heap_t h = { 0 };
    p->port->heap(p->ctx, &h);
    char msg[ESPOS_HEALTH_MSG_MAX];
    uint32_t flags = 0;
    espos_health_state_t st = espos_health_policy_memory(&p->cfg, &h, msg, sizeof(msg), &flags);
    p->port->report(p->ctx, "lowMemory", st, msg, flags);

    /* Not before the first watch: a firmware that watches nothing should not
     * spend a condition slot (and a NORMAL notification) on taskStalled. */
    if (p->ever_watched) {
        check_tasks(p, now);
    }

    /* The table decides, not the reports above: a consumer's own fatal
     * condition (a silent N2K bus) counts exactly like lowMemory does. */
    espos_health_condition_t c;
    if (!p->port->fatal_alarm(p->ctx, &c)) {
        p->strikes = 0;
        return 0;
    }
    p->fatal = c;
    p->strikes++;
    if (p->strikes < p->cfg.strikes) {
        return p->strikes;
    }

    espos_health_reset_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = RECORD_MAGIC;
    snprintf(rec.key, sizeof(rec.key), "%s", c.key);
    /* Clipped on purpose: the record keeps 63 of the message's 95 characters. */
    size_t n = strnlen(c.message, sizeof(rec.message) - 1);
    memcpy(rec.message, c.message, n);
    rec.message[n] = '\0';
    rec.min_free_heap = h.total_min;
    rec.min_internal = h.internal_min;
    rec.largest_block = h.largest_block;
    rec.uptime_s = p->port->uptime_s(p->ctx);
    rec.unix_ms = p->port->unix_ms(p->ctx);
    p->restarting = true;
    p->port->store_record(p->ctx, &rec);
    p->port->restart(p->ctx);
    return p->strikes;
}

static espos_health_watched_t *find(espos_health_policy_t *p, void *task)
{
    for (size_t i = 0; i < ESPOS_HEALTH_WATCHED_MAX; i++) {
        if (p->watched[i].task == task) {
            return &p->watched[i];
        }
    }
    return NULL;
}

esp_err_t espos_health_policy_watch(espos_health_policy_t *p, void *task, const char *name, uint32_t timeout_ms)
{
    if (!task || !name || !name[0] || !p->port) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_health_watched_t *w = find(p, task);
    if (!w) {
        w = find(p, NULL);
        if (!w) {
            return ESP_ERR_NO_MEM;
        }
    }
    /* Fill before publishing the identity: kick() scans without a lock and
     * must never see a half-written slot. Re-watching keeps the old stamp
     * out of the picture too — the task is alive, it is calling us. */
    snprintf(w->name, sizeof(w->name), "%s", name);
    w->timeout_ms = timeout_ms;
    w->last_kick_ms = p->port->now_ms(p->ctx);
    w->task = task;
    p->ever_watched = true;
    return ESP_OK;
}

esp_err_t espos_health_policy_unwatch(espos_health_policy_t *p, void *task)
{
    espos_health_watched_t *w = task ? find(p, task) : NULL;
    if (!w) {
        return ESP_ERR_NOT_FOUND;
    }
    w->task = NULL; /* a tombstone, not a compaction: kick() may be scanning */
    return ESP_OK;
}

void espos_health_policy_kick(espos_health_policy_t *p, void *task)
{
    if (!p->port || !task) {
        return;
    }
    espos_health_watched_t *w = find(p, task);
    if (w) {
        w->last_kick_ms = p->port->now_ms(p->ctx);
    }
}
