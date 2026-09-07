/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_time policy — the clock's decisions as pure C.
 *
 * Three things about a wall clock are worth getting right and none of them
 * need a platform: which source is allowed to overwrite which (ranking), when
 * a time carried through a deep sleep has aged past usefulness (staleness),
 * and how an instant is written down (ISO 8601 UTC with milliseconds). All
 * three live here, driven by an injected monotonic clock, so a host test can
 * step time forward by a day without sleeping — the same shape as
 * espos_wifi_sm, espos_sk_token_sm and espos_health_policy.
 *
 * The instant itself is kept as an offset: `epoch_at_zero_ms` is what the wall
 * clock read when the monotonic counter was zero. Reading the time is then one
 * addition, and — the point of the exercise — a time learned late still dates
 * an event that happened early, because the offset applies to every monotonic
 * stamp ever taken, not only to the ones after the sync.
 *
 * Threading: none. The caller (espos_time.c) serialises access under its own
 * mutex; nothing here allocates, blocks or calls out.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#include "espos_time.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Everything the policy needs from the outside world. */
typedef struct {
    /* Monotonic milliseconds since boot. 64-bit on purpose: this one must not
     * wrap, because it is the base every wall-clock reading is built on. */
    int64_t (*now_ms)(void *ctx);
} espos_time_policy_port_t;

typedef struct espos_time_policy {
    const espos_time_policy_port_t *port;
    void *ctx;
    espos_time_src_t src;      /* NONE until something sets the clock */
    int64_t epoch_at_zero_ms;  /* unix ms the monotonic counter's zero corresponds to */
    int64_t set_at_mono_ms;    /* when the current source last set it */
    int64_t prior_age_ms;      /* age the value already had when it was adopted (a deep sleep) */
    uint32_t stale_after_h;    /* an RTC time older than this is not "synced"; 0 = never stale */
    uint32_t sets;             /* how often a source set the clock, for the status document */
} espos_time_policy_t;

/**
 * `stale_after_h` is CONFIG_ESPOS_TIME_RTC_STALE_H on a device: how long a
 * time restored from RTC memory across a deep sleep still counts as known.
 * 0 disables the staleness rule entirely.
 */
void espos_time_policy_init(espos_time_policy_t *p, const espos_time_policy_port_t *port, void *ctx,
                            uint32_t stale_after_h);

/**
 * Offer `unix_ms` from `src`. Accepted when no clock is set, when `src` ranks
 * at or above the source that set the current one (so a source may always
 * refresh itself), and refused with ESP_ERR_INVALID_STATE otherwise — that is
 * the whole of "a lower-ranked source never overrides SNTP once it synced".
 * ESP_ERR_INVALID_ARG for a non-positive `unix_ms` or an out-of-range src.
 */
esp_err_t espos_time_policy_set(espos_time_policy_t *p, int64_t unix_ms, espos_time_src_t src);

/**
 * espos_time_policy_set() for a value that was already `prior_age_ms` old when
 * it arrived — the deep-sleep case, where the instant in RTC memory was
 * recorded before the nap. The staleness rule counts that age too, so a device
 * that sleeps for two days does not wake up calling a two-day-old clock fresh.
 * `prior_age_ms` below 0 is treated as 0.
 */
esp_err_t espos_time_policy_set_aged(espos_time_policy_t *p, int64_t unix_ms, espos_time_src_t src,
                                     int64_t prior_age_ms);

/** Unix milliseconds now, or 0 when no source has set the clock. */
int64_t espos_time_policy_now_ms(const espos_time_policy_t *p);

/**
 * What the clock read at monotonic stamp `mono_ms` — the call that makes a
 * buffered value keep its own time. Correct even when the sync happened after
 * `mono_ms` was taken, because the offset applies to the whole timeline.
 * 0 when unsynced.
 */
int64_t espos_time_policy_at_ms(const espos_time_policy_t *p, int64_t mono_ms);

/**
 * Does the device know what time it is? True for any source that set the
 * clock this boot; for SRC_RTC also only while the restored time is younger
 * than `stale_after_h`. A stale RTC time is still returned by now_ms() — it
 * is the best guess available and better than nothing for a log prefix — but
 * it is not represented as synced, and it does not stop a real source from
 * replacing it.
 */
bool espos_time_policy_is_synced(const espos_time_policy_t *p);

/**
 * Write `unix_ms` as "2026-09-07T10:12:13.456Z" into buf and return the
 * length written. `unix_ms` of 0 or below, or a buffer shorter than
 * ESPOS_TIME_ISO_MAX, yields "" and 0. Pure: no locale, no libc time
 * conversion, no timezone — a fixed civil-calendar computation, which is also
 * why it is testable on any host and identical on every target.
 */
size_t espos_time_iso8601_format(int64_t unix_ms, char *buf, size_t n);

/**
 * Parse an ISO 8601 timestamp — "2026-09-07T10:12:13.456Z", the shape a
 * SignalK server puts on navigation.datetime — into unix milliseconds.
 * Accepts a missing fractional part, any number of fractional digits, and a
 * numeric offset ("+02:00") as well as "Z". Returns 0 on anything it does not
 * understand, which callers treat as "no time in this value".
 */
int64_t espos_time_iso8601_parse(const char *s);

#ifdef __cplusplus
}
#endif
