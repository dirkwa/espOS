/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sched — the timer wheel. See espos_sched.h for the contract.
 *
 * A linear scan over a fixed table, not a heap. With ESPOS_FLOW_MAX_TIMERS at
 * 32 the scan is 32 comparisons of a subtraction; a binary heap would be
 * fewer instructions and considerably more code to get wrong around the wrap,
 * and the loop runs this at most once per millisecond. If a firmware ever
 * wants hundreds of timers, replace the scan here and nothing above it
 * changes — that is the reason this file has no other job.
 */
#include <string.h>

#include "espos_sched.h"

/* A handle packs slot index and generation so a stale handle is detectable:
 * index in the low 16 bits, generation in the high 16, and never all-zero
 * (generation starts at 1). */
#define H_MAKE(idx, gen) (((uint32_t)(gen) << 16) | (uint32_t)((idx) & 0xFFFFu))
#define H_IDX(h)         ((size_t)((h) & 0xFFFFu))
#define H_GEN(h)         ((uint16_t)((h) >> 16))

esp_err_t espos_sched_init(espos_sched_t *s, espos_sched_timer_t *slots, size_t cap)
{
    if (!s || !slots || cap == 0 || cap > 0xFFFFu) return ESP_ERR_INVALID_ARG;
    memset(s, 0, sizeof(*s));
    memset(slots, 0, cap * sizeof(slots[0]));
    s->slots = slots;
    s->cap = cap;
    s->seq = 1;
    return ESP_OK;
}

esp_err_t espos_sched_add(espos_sched_t *s, uint32_t now_ms, uint32_t delay_ms, uint32_t period_ms,
                          espos_sched_cb_t cb, void *arg, espos_sched_handle_t *out)
{
    if (!s || !s->slots || !cb || !out) return ESP_ERR_INVALID_ARG;
    if (delay_ms > ESPOS_SCHED_MAX_DELAY_MS || period_ms > ESPOS_SCHED_MAX_DELAY_MS) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0; i < s->cap; i++) {
        espos_sched_timer_t *t = &s->slots[i];
        if (t->used) continue;

        uint16_t gen = (uint16_t)(s->seq++ & 0xFFFFu);
        if (gen == 0) gen = (uint16_t)(s->seq++ & 0xFFFFu); /* 0 would make an all-zero handle */
        t->cb = cb;
        t->arg = arg;
        t->due_ms = now_ms + delay_ms;
        t->period_ms = period_ms;
        t->gen = gen;
        t->used = true;
        t->cancelled = false;
        t->swept = false;
        /* Added from inside a callback: this sweep has already decided what it
         * is running, so a zero-delay timer armed here waits for the next one.
         * Without this a callback that re-arms itself with delay 0 spins
         * inside fire() forever and the loop never reads its mailbox again. */
        t->fresh = s->firing;
        *out = H_MAKE(i, gen);
        return ESP_OK;
    }
    *out = ESPOS_SCHED_HANDLE_NONE;
    return ESP_ERR_NO_MEM;
}

/* Resolve a handle to a live, uncancelled slot. NULL for anything stale. */
static espos_sched_timer_t *resolve(const espos_sched_t *s, espos_sched_handle_t h)
{
    if (h == ESPOS_SCHED_HANDLE_NONE) return NULL;
    size_t idx = H_IDX(h);
    if (idx >= s->cap) return NULL;
    espos_sched_timer_t *t = &s->slots[idx];
    if (!t->used || t->cancelled || t->gen != H_GEN(h)) return NULL;
    return t;
}

esp_err_t espos_sched_cancel(espos_sched_t *s, espos_sched_handle_t h)
{
    if (!s || !s->slots) return ESP_ERR_INVALID_ARG;
    espos_sched_timer_t *t = resolve(s, h);
    if (!t) return ESP_ERR_NOT_FOUND;

    /* Mark, do not free: fire() may be iterating this very slot, and a
     * callback is allowed to cancel itself. Outside a sweep the slot can be
     * released at once; inside one, fire() reaps it on the way out. */
    t->cancelled = true;
    if (!s->firing) t->used = false;
    return ESP_OK;
}

uint32_t espos_sched_next_due(const espos_sched_t *s, uint32_t now_ms)
{
    if (!s || !s->slots) return UINT32_MAX;

    uint32_t best = UINT32_MAX;
    for (size_t i = 0; i < s->cap; i++) {
        const espos_sched_timer_t *t = &s->slots[i];
        if (!t->used || t->cancelled) continue;
        /* Modular distance, so an overdue timer reads as 0 rather than as
         * nearly 4 billion milliseconds away. */
        uint32_t in = espos_sched_reached(now_ms, t->due_ms) ? 0u : (t->due_ms - now_ms);
        if (in < best) best = in;
        if (best == 0) break;
    }
    return best;
}

size_t espos_sched_fire(espos_sched_t *s, uint32_t now_ms)
{
    if (!s || !s->slots || s->firing) return 0;

    s->firing = true;

    size_t fired = 0;
    for (;;) {
        /* Pick the earliest due timer that has not run in this sweep. Equal
         * deadlines resolve by slot index, which is insertion order for a
         * table that hands out the lowest free slot. */
        espos_sched_timer_t *pick = NULL;
        for (size_t i = 0; i < s->cap; i++) {
            espos_sched_timer_t *t = &s->slots[i];
            if (!t->used || t->cancelled || t->swept || t->fresh) continue;
            if (!espos_sched_reached(now_ms, t->due_ms)) continue;
            if (!pick || (int32_t)(t->due_ms - pick->due_ms) < 0) pick = t;
        }
        if (!pick) break;

        pick->swept = true;

        espos_sched_cb_t cb = pick->cb;
        void *arg = pick->arg;
        uint32_t period = pick->period_ms;

        if (period == 0) {
            /* A one-shot is retired BEFORE the callback runs, so the callback
             * may re-add itself into the same slot and so cancelling an
             * already-fired handle answers NOT_FOUND rather than cancelling
             * whatever took the slot next. */
            pick->cancelled = true;
        } else {
            /* Next deadline from the deadline just met, not from now: no
             * drift. Skip whole periods the loop slept through rather than
             * firing them back to back. */
            uint32_t next = pick->due_ms + period;
            if (espos_sched_reached(now_ms, next)) {
                uint32_t behind = now_ms - pick->due_ms;
                uint32_t skip = behind - (behind % period) + period;
                next = pick->due_ms + skip;
            }
            pick->due_ms = next;
        }

        cb(arg);
        fired++;
    }

    /* Unwind: reap what was cancelled or spent while we were iterating, and
     * clear the sweep marks. */
    s->firing = false;
    for (size_t i = 0; i < s->cap; i++) {
        espos_sched_timer_t *t = &s->slots[i];
        t->swept = false;
        t->fresh = false;
        if (t->used && t->cancelled) t->used = false;
    }
    return fired;
}

size_t espos_sched_count(const espos_sched_t *s)
{
    if (!s || !s->slots) return 0;
    size_t n = 0;
    for (size_t i = 0; i < s->cap; i++) {
        if (s->slots[i].used && !s->slots[i].cancelled) n++;
    }
    return n;
}
