/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * espos_health — the device's own view of what is wrong with it.
 *
 * A component that notices a condition it cannot fix — a wake service that
 * went away, internal RAM running out, a bus that stopped answering — raises
 * it here with a short stable key. Something else decides what to do with it:
 * espos_sk turns conditions into SignalK notifications, and any application
 * may add a sink of its own (a red LED, a line on a display, a relay).
 *
 * This exists so that "report a problem" is not a reason to depend on the
 * SignalK stack. espos_voice used to call espos_sk_notify() directly for its
 * one notification, which made a voice satellite unbuildable without SignalK
 * and pointed the dependency graph the wrong way — an optional component
 * depending on another optional component for a core concern. Reporting is
 * the core concern; SignalK is one sink.
 *
 * Conditions are level-triggered and idempotent: report the same state and
 * message twice and sinks are called once, so a caller may re-report on every
 * poll of whatever it is watching. ESPOS_HEALTH_NORMAL clears a condition and
 * is delivered like any other change — it is what retires an alert a previous
 * boot raised.
 *
 * Thread-safe. Sinks run on the reporting task with no lock held, so a sink
 * may report conditions of its own; it must not block for long.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_HEALTH_NORMAL = 0,  /* condition cleared */
    ESPOS_HEALTH_WARN,
    ESPOS_HEALTH_ALARM,
} espos_health_state_t;

/* A key is an identifier, not a sentence ("lowMemory", "wakeService"): it is
 * what a rule or a dashboard keys on, and it ends up in a SignalK path. The
 * message is the human-readable half and may change without re-notifying. */
#define ESPOS_HEALTH_KEY_MAX 24
#define ESPOS_HEALTH_MSG_MAX 96

/**
 * Raise (or with ESPOS_HEALTH_NORMAL, clear) the condition `key`.
 *
 * Sinks are called only when the state or the message actually changed.
 * `message` may be NULL or "".
 *
 * @return ESP_OK also when nothing changed; ESP_ERR_INVALID_ARG for an empty
 *         key; ESP_ERR_INVALID_SIZE when key/message exceed the maxima above
 *         (rejected rather than truncated — a clipped key would never match on
 *         the next call, so every report would consume another slot);
 *         ESP_ERR_NO_MEM when CONFIG_ESPOS_HEALTH_MAX_CONDITIONS is exhausted.
 */
esp_err_t espos_health_report(const char *key, espos_health_state_t state, const char *message);

/** Human-readable state, for logs and sinks: "normal", "warn", "alarm". */
const char *espos_health_state_str(espos_health_state_t state);

/* ------------------------------------------------------------------ sinks */

typedef void (*espos_health_sink_t)(const char *key, espos_health_state_t state,
                                    const char *message, void *arg);

/**
 * Register a sink. Every condition recorded so far is replayed into it before
 * this returns, so a sink that comes up late (espos_sk connects long after the
 * first report) still learns the current state instead of waiting for the next
 * change.
 *
 * @return ESP_ERR_NO_MEM when CONFIG_ESPOS_HEALTH_MAX_SINKS is exhausted,
 *         ESP_ERR_INVALID_STATE if (sink, arg) is already registered.
 */
esp_err_t espos_health_add_sink(espos_health_sink_t sink, void *arg);

/**
 * Remove a sink. A call already in flight on another task may still complete
 * after this returns.
 */
esp_err_t espos_health_remove_sink(espos_health_sink_t sink, void *arg);

/* -------------------------------------------------------------- inspection */

typedef struct {
    char key[ESPOS_HEALTH_KEY_MAX];
    espos_health_state_t state;
    char message[ESPOS_HEALTH_MSG_MAX];
} espos_health_condition_t;

/**
 * Copy the current conditions into `out` (at most `max`).
 * @param out may be NULL to query the count only.
 * @return how many conditions exist, which may exceed `max`.
 */
size_t espos_health_snapshot(espos_health_condition_t *out, size_t max);

/** Worst state currently recorded — what a single status LED wants to know. */
espos_health_state_t espos_health_worst(void);

/** Forget every condition and sink (tests). */
void espos_health_reset(void);

#ifdef __cplusplus
}
#endif
