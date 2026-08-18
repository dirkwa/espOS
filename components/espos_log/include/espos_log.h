/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_log — an in-RAM ring of recent log lines.
 *
 * Hooks esp_log's vprintf so everything that reaches the console is also
 * kept (colour codes stripped, one record per line) with a monotonically
 * increasing sequence number, so a client can page: "give me lines after
 * seq N". Oldest lines are overwritten when the ring is full.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Install the hook. Idempotent. Lines logged before this call are lost. */
esp_err_t espos_log_init(void);

/** Uninstall the hook (tests). The ring content is freed. */
void espos_log_deinit(void);

typedef struct {
    uint32_t first;     /* oldest sequence number still in the ring */
    uint32_t next;      /* sequence the next line will get */
    size_t used;        /* bytes in use */
    size_t size;        /* ring capacity */
    uint32_t dropped;   /* lines overwritten so far */
} espos_log_stats_t;

void espos_log_stats(espos_log_stats_t *out);

/**
 * Visit lines with seq > after, oldest first, at most limit (0 = all).
 * The callback runs with the ring locked: copy, do not log, do not block.
 * Return false from the callback to stop early. Returns the number of
 * lines visited.
 */
typedef bool (*espos_log_visit_cb_t)(uint32_t seq, const char *line, size_t len, void *arg);
size_t espos_log_read(uint32_t after, size_t limit, espos_log_visit_cb_t cb, void *arg);

/** Called (from a timer task, never from the logging call itself) at
 * most every ESPOS_LOG_NOTIFY_MS when new lines arrived. */
typedef void (*espos_log_notify_cb_t)(uint32_t next_seq, void *arg);
void espos_log_set_notify(espos_log_notify_cb_t cb, void *arg);

/** Set a tag's (or "*") log level by name: none error warn info debug verbose. */
esp_err_t espos_log_set_level(const char *tag, const char *level);
const char *espos_log_level_name(int level);

#ifdef __cplusplus
}
#endif
