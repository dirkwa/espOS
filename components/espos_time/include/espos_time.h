/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_time — the device's wall clock, and the reason a delta can carry a
 * timestamp at all.
 *
 * An ESP32 boots with no idea what time it is. Until something tells it, the
 * only clock it has is a monotonic millisecond counter that starts at zero,
 * which is enough to schedule work and useless for saying when a measurement
 * was taken. That gap is not academic: values a device buffers while the
 * server is unreachable are replayed minutes or hours later, and a delta
 * without a timestamp is stamped by the server with the time it arrived — so
 * an hour of wind data recorded during an outage lands in the log as one
 * burst at reconnect, in the wrong place and the wrong order.
 *
 * This component closes it. It keeps one wall clock, learned from whichever
 * source got there first and is ranked highest (SNTP over a manual set over
 * the SignalK stream over an RTC value carried through a deep sleep), and
 * hands it out as unix milliseconds or as an ISO 8601 string. Everything
 * else — the delta engine's timestamps, the log's wall-clock prefix, the
 * `at` field of a reset record — reads it from here.
 *
 * The clock may be wrong or absent, and callers must cope: espos_time_now_ms()
 * returns 0 while unsynced rather than a plausible-looking 1970, so a consumer
 * that forgets to check produces an obviously missing value instead of a
 * quietly wrong one.
 *
 * Timezone stance: SignalK is UTC and so is everything this component
 * publishes. A local time is a display concern — a panel showing the crew what
 * o'clock it is — so espos_time_set_tz() and espos_time_parts() exist for a UI
 * to use, and nothing in the data path ever looks at them.
 *
 * Threading: every getter is a lock-protected snapshot copy and never waits on
 * a network. The SNTP sync callback runs on IDF's SNTP task, espos_time_set()
 * on whatever task the source lives on, and subscriber callbacks run on that
 * same task with no espos_time lock held: copy what you need and return, never
 * block there.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest string espos_time_iso8601() writes, NUL included:
 * "2026-09-07T10:12:13.456Z" is 24 characters. Part of the ABI. */
#define ESPOS_TIME_ISO_MAX 25

/* Longest POSIX TZ string espos_time_set_tz() accepts, NUL included.
 * "CET-1CEST,M3.5.0,M10.5.0/3" and friends fit comfortably. */
#define ESPOS_TIME_TZ_MAX 40

/**
 * Where the current time came from. Also the ranking: a source never
 * overrides one above it, so a coarse SignalK timestamp cannot walk back over
 * a clock SNTP already disciplined, while any source at all beats none.
 * Values are part of the ABI: append before _MAX, never renumber.
 */
typedef enum {
    ESPOS_TIME_SRC_NONE = 0,   /* no clock: espos_time_now_ms() returns 0 */
    ESPOS_TIME_SRC_RTC = 1,    /* carried through a deep sleep in RTC memory */
    ESPOS_TIME_SRC_SK = 2,     /* navigation.datetime from the SignalK stream */
    ESPOS_TIME_SRC_MANUAL = 3, /* PUT /api/v1/time, or an application call */
    ESPOS_TIME_SRC_SNTP = 4,   /* an NTP server */
    ESPOS_TIME_SRC_MAX = 5,
} espos_time_src_t;

/**
 * Start the clock: read the configuration, adopt an RTC value this boot may
 * have inherited, register the /time endpoints and — when time.sntp is on —
 * arm SNTP so it starts on the first ESPOS_EVENT_NETWORK_UP. Requires
 * espos_init() (config) and espos_httpd_start(); call it after
 * espos_net_start() so the first NETWORK_UP is not missed. Idempotent.
 */
esp_err_t espos_time_start(void);

/**
 * True when the device believes it knows the time: a source set the clock
 * this boot, or an RTC value survived a deep sleep and is not yet stale
 * (CONFIG_ESPOS_TIME_RTC_STALE_H). False before espos_time_start().
 */
bool espos_time_is_synced(void);

/** Unix milliseconds UTC, or 0 when unsynced — never a plausible-looking
 * 1970, so a caller that forgets to check produces a missing value rather
 * than a wrong one. */
int64_t espos_time_now_ms(void);

/** Which source the current time came from; NONE while unsynced. */
espos_time_src_t espos_time_source(void);

/** Name of a source as the REST document spells it: "none", "rtc", "sk",
 * "manual", "sntp". */
const char *espos_time_src_str(espos_time_src_t src);

/**
 * Write the current time as "2026-09-07T10:12:13.456Z" into buf (needs
 * ESPOS_TIME_ISO_MAX bytes) and return its length. Writes "" and returns 0
 * while unsynced — the same "obviously missing" rule as now_ms().
 */
size_t espos_time_iso8601(char *buf, size_t n);

/**
 * Format an explicit unix-millisecond instant the same way. `unix_ms` of 0 or
 * below yields "" and 0, so a caller can pass a stored stamp straight through
 * without testing it first.
 */
size_t espos_time_iso8601_of(int64_t unix_ms, char *buf, size_t n);

/**
 * Tell the clock what time it is. Used by the SignalK fallback, by
 * PUT /api/v1/time and by an application with a GPS or an RTC chip of its
 * own. Refused with ESP_ERR_INVALID_STATE when a strictly higher-ranked
 * source already set the clock this boot, so a coarse source can never
 * degrade a good one; the same source may always refresh itself.
 * ESP_ERR_INVALID_ARG for a non-positive `unix_ms` or a src outside the enum.
 * On success the subscribers run on the calling task before it returns.
 */
esp_err_t espos_time_set(int64_t unix_ms, espos_time_src_t src);

/**
 * Set the POSIX timezone string used by espos_time_parts() — "UTC0",
 * "CET-1CEST,M3.5.0,M10.5.0/3" — and persist it in time.tz. Purely a display
 * concern: nothing espOS publishes is ever in local time. NULL or "" resets
 * to UTC. ESP_ERR_INVALID_SIZE beyond ESPOS_TIME_TZ_MAX.
 */
esp_err_t espos_time_set_tz(const char *posix_tz);

/** The configured POSIX timezone string; "UTC0" when none was set. */
const char *espos_time_tz(void);

/**
 * Broken-down local time, for a display. Deliberately not `struct tm`: this
 * header is the C ABI a binding is generated from and stays free of platform
 * types. `wday` is 0 = Sunday, `yday` 0-365, `utc_offset_s` the offset the
 * configured timezone applies at that instant (0 under the default UTC0).
 */
typedef struct {
    int32_t year;         /* full year, e.g. 2026 */
    uint8_t month;        /* 1-12 */
    uint8_t day;          /* 1-31 */
    uint8_t hour;         /* 0-23 */
    uint8_t minute;       /* 0-59 */
    uint8_t second;       /* 0-60, a leap second included */
    uint16_t millisecond; /* 0-999 */
    uint8_t wday;         /* 0 = Sunday */
    uint16_t yday;        /* 0-365 */
    int32_t utc_offset_s; /* seconds east of UTC at this instant */
} espos_time_parts_t;

/**
 * Fill `out` with the current time in the configured timezone.
 * ESP_ERR_INVALID_STATE while unsynced (out is left untouched).
 */
esp_err_t espos_time_parts(espos_time_parts_t *out);

/**
 * Called whenever a source sets or refreshes the clock. Runs on that source's
 * task — IDF's SNTP task for SRC_SNTP, the SignalK stream task for SRC_SK,
 * the HTTP server's task for a manual PUT — with no espos_time lock held.
 * Copy what you need and return; never block, and never call back into a
 * component that might be waiting for your task. `arg` is handed back
 * untouched. Small fixed table: ESP_ERR_NO_MEM when full; the same (cb, arg)
 * pair registered twice is called once. Callable before espos_time_start().
 */
typedef void (*espos_time_cb_t)(espos_time_src_t src, void *arg);
esp_err_t espos_time_subscribe(espos_time_cb_t cb, void *arg);
esp_err_t espos_time_unsubscribe(espos_time_cb_t cb, void *arg);

/**
 * The clock as the JSON document of docs/rest-api.md (malloc'ed; caller
 * frees). ESP_ERR_INVALID_STATE before espos_time_start().
 */
esp_err_t espos_time_status_json(char **out_json);

/**
 * espos_time_now_ms() with the signature a consumer's clock hook wants
 * (`int64_t (*)(void *)`, the shape espos_sk_delta_set_clock() takes). `arg`
 * is ignored. Exists so espos_sk can pass the clock across without inventing
 * a trampoline of its own.
 */
int64_t espos_time_now_ms_or_zero(void *arg);

#ifdef __cplusplus
}
#endif
