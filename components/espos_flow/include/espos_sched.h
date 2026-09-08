/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_sched — the timer wheel behind espos_flow, as pure C.
 *
 * Deciding what is due next is the one part of a scheduler worth testing
 * exhaustively and the one part that needs no platform at all: given a set of
 * deadlines and a clock reading, which timer fires, in what order, and how
 * long may the loop sleep before the next one. None of that wants FreeRTOS or
 * esp_timer, so none of it is here — the caller reads the clock and calls
 * espos_sched_fire(). The same shape as espos_wifi_sm, espos_sk_token_sm,
 * espos_health_policy and espos_time_policy: a host test drives a whole day of
 * timers in microseconds and can single-step the millisecond the counter wraps.
 *
 * Time is uint32_t milliseconds, because that is what a device's monotonic
 * counter is cheapest in and what the flow API takes. It wraps every 49.7
 * days, and a device that runs a season must survive that: every comparison
 * in here is a subtraction interpreted as a signed 32-bit difference
 * ((int32_t)(a - b) < 0), never a < b. A deadline is "due" when that
 * difference says it is at or behind the clock, so a wrap is not a special
 * case, it is arithmetic that was always modular. The one thing this costs:
 * no timer may be scheduled more than 2^31-1 ms (~24.8 days) ahead, which
 * espos_sched_add() rejects rather than silently firing it immediately half a
 * lifetime later.
 *
 * Threading: none. Everything here runs on whatever task calls it; the caller
 * (espos_flow.c) owns the mutex. Nothing allocates, blocks or logs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The furthest ahead a timer may be scheduled. Past this the signed
 * difference that orders deadlines changes sign and "later" would read as
 * "overdue" — the classic wrap bug, rejected at the door instead. */
#define ESPOS_SCHED_MAX_DELAY_MS 0x7FFFFFFFu

/* A timer's identity, returned by espos_sched_add(). Never 0 for a live
 * timer, so 0 is a usable "no timer" value in application structs. Handles
 * carry a generation counter, so cancelling a slot that has since been reused
 * by a different timer cancels nothing rather than the innocent newcomer. */
typedef uint32_t espos_sched_handle_t;

#define ESPOS_SCHED_HANDLE_NONE ((espos_sched_handle_t)0)

/* Fired on the caller's task by espos_sched_fire(). A callback may add and
 * cancel timers, including its own handle. */
typedef void (*espos_sched_cb_t)(void *arg);

typedef struct {
    espos_sched_cb_t cb;
    void *arg;
    uint32_t due_ms;    /* absolute, modular */
    uint32_t period_ms; /* 0 = one-shot */
    uint16_t gen;       /* bumped on every reuse of the slot */
    bool used;
    bool cancelled;  /* cancelled from inside its own callback; reaped by fire() */
    bool swept;      /* already run in the espos_sched_fire() now in progress */
    bool fresh;      /* added by a callback during that sweep: not eligible in it */
} espos_sched_timer_t;

/* Fixed table, sized by the caller: no allocation, and the number of timers a
 * firmware can hold is decided at build time like every other espOS table. */
typedef struct {
    espos_sched_timer_t *slots;
    size_t cap;
    uint32_t seq; /* generation source */
    bool firing;  /* inside espos_sched_fire(): defers slot reuse */
} espos_sched_t;

/**
 * Bind a scheduler to a caller-owned array of `cap` slots. The array's
 * contents are cleared. ESP_ERR_INVALID_ARG for a NULL argument or cap 0.
 */
esp_err_t espos_sched_init(espos_sched_t *s, espos_sched_timer_t *slots, size_t cap);

/**
 * Schedule `cb` to run `delay_ms` after `now_ms`, then every `period_ms` (0 =
 * one-shot). A `delay_ms` of 0 is due immediately — the next espos_sched_fire()
 * runs it.
 *
 * A periodic timer's next deadline is computed from the deadline it just met,
 * not from the moment the callback finished, so a slow callback does not make
 * a 1000 ms timer drift into a 1050 ms one. If the loop was blocked long
 * enough to miss whole periods, the missed ones are dropped rather than
 * fired back to back (a catch-up burst is never what a sensor poll wants).
 *
 * @param out receives the handle; may not be NULL.
 * @return ESP_ERR_NO_MEM when every slot is in use, ESP_ERR_INVALID_ARG for a
 *         NULL callback or a delay/period beyond ESPOS_SCHED_MAX_DELAY_MS.
 */
esp_err_t espos_sched_add(espos_sched_t *s, uint32_t now_ms, uint32_t delay_ms, uint32_t period_ms,
                          espos_sched_cb_t cb, void *arg, espos_sched_handle_t *out);

/**
 * Cancel a timer. Safe from inside any callback, including the timer's own —
 * the slot is only marked, and reaped when espos_sched_fire() unwinds, so a
 * callback cancelling itself does not have its arguments pulled out from
 * under the loop that is iterating the table.
 *
 * @return ESP_ERR_NOT_FOUND for a handle that never existed, was already
 *         cancelled, or belongs to a one-shot that has already fired. That is
 *         not an error a caller has to handle: it is the answer to "cancel
 *         this if it is still pending".
 */
esp_err_t espos_sched_cancel(espos_sched_t *s, espos_sched_handle_t h);

/**
 * Milliseconds until the earliest deadline, given the clock reads `now_ms`.
 * 0 when something is already due (or overdue), UINT32_MAX when no timer is
 * pending — which is the loop's "block until something is posted" signal.
 */
uint32_t espos_sched_next_due(const espos_sched_t *s, uint32_t now_ms);

/**
 * Run every timer due at `now_ms` and return how many fired.
 *
 * Order among timers due at the same moment is by deadline, earliest first,
 * and among equal deadlines by insertion — the order they were added, which
 * is the order a wiring function created them in and therefore the only order
 * a reader can predict.
 *
 * A timer added by a callback is not fired in the same sweep even if it is
 * already due; it goes on the next one. Otherwise a callback that re-arms
 * itself with delay 0 would spin the loop forever inside one fire().
 *
 * Re-entering fire() from a callback is refused (returns 0): the loop calls
 * it, callbacks do not.
 */
size_t espos_sched_fire(espos_sched_t *s, uint32_t now_ms);

/** Live timers (added, not yet cancelled or fired-and-done). */
size_t espos_sched_count(const espos_sched_t *s);

/**
 * True when `a` is at or after `b` on a modular 32-bit millisecond clock.
 * Exposed because it is the whole of the wrap-safety argument and a test
 * should be able to aim at it directly.
 */
static inline bool espos_sched_reached(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) >= 0;
}

#ifdef __cplusplus
}
#endif
