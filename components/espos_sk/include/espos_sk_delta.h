/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Delta batcher + offline ring buffer — pure C, no OS calls, host-testable.
 *
 *   publish(path, value)  →  pending (last value per path wins)
 *   after batch_ms         →  one delta message {"context":"vessels.self","updates":[...]}
 *   connected              →  message goes out (backlog drains oldest-first, rate-limited)
 *   offline                →  message enters the ring (bounded by count and bytes, oldest dropped)
 *
 * The owner calls espos_sk_delta_take() from its sender task and transmits
 * what it gets; espos_sk_delta_requeue() puts a message back if the send
 * failed. Everything else is bookkeeping.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESPOS_SK_PATH_MAX 96
#define ESPOS_SK_VALUE_MAX 256
#define ESPOS_SK_PENDING_MAX 32

typedef struct {
    const char *label;        /* source label, e.g. the hostname */
    uint32_t batch_ms;        /* coalescing window (default 100) */
    size_t max_msgs;          /* ring capacity in messages */
    size_t max_bytes;         /* ring capacity in bytes (sum of message lengths) */
    uint32_t drain_per_s;     /* max backlog messages per second while draining */
} espos_sk_delta_cfg_t;

typedef struct {
    size_t pending;           /* values waiting for the batch window */
    size_t buffered;          /* messages in the ring */
    size_t buffered_bytes;
    uint32_t dropped;         /* messages dropped because the ring was full */
    uint32_t built;           /* messages built */
    uint32_t taken;           /* messages handed to the sender */
} espos_sk_delta_stats_t;

typedef struct espos_sk_delta espos_sk_delta_t;

espos_sk_delta_t *espos_sk_delta_create(const espos_sk_delta_cfg_t *cfg);
void espos_sk_delta_destroy(espos_sk_delta_t *d);

/** value_json is a complete JSON value (number, "string", true, {...}). */
esp_err_t espos_sk_delta_publish(espos_sk_delta_t *d, const char *path, const char *value_json, uint32_t now_ms);

/** Sender side. Returns a malloc'ed message to transmit now, or NULL.
 * connected=false only batches into the ring. */
char *espos_sk_delta_take(espos_sk_delta_t *d, uint32_t now_ms, bool connected);
/** Put a message back at the head (send failed); takes ownership. */
void espos_sk_delta_requeue(espos_sk_delta_t *d, char *msg);
/** Force the pending batch into a message now (e.g. before disconnect). */
void espos_sk_delta_flush(espos_sk_delta_t *d, uint32_t now_ms);
/** ms until something is due (batch window / drain gap); UINT32_MAX if idle. */
uint32_t espos_sk_delta_next_due_ms(const espos_sk_delta_t *d, uint32_t now_ms, bool connected);
void espos_sk_delta_stats(const espos_sk_delta_t *d, espos_sk_delta_stats_t *out);
void espos_sk_delta_set_label(espos_sk_delta_t *d, const char *label);
/** Change batching window / drain rate on a live engine. */
void espos_sk_delta_set_timing(espos_sk_delta_t *d, uint32_t batch_ms, uint32_t drain_per_s);

/* Helpers: format values as JSON into buf. */
int espos_sk_json_number(char *buf, size_t size, double v);
int espos_sk_json_string(char *buf, size_t size, const char *s);

#ifdef __cplusplus
}
#endif
