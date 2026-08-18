/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espos_sk_delta.h"

typedef struct {
    char path[ESPOS_SK_PATH_MAX];
    char value[ESPOS_SK_VALUE_MAX];
} pending_t;

struct espos_sk_delta {
    espos_sk_delta_cfg_t cfg;
    char label[40];
    pending_t pending[ESPOS_SK_PENDING_MAX];
    size_t pending_n;
    uint32_t pending_since_ms;
    char **ring;              /* messages, oldest at head */
    size_t head, count;
    size_t bytes;
    uint32_t last_take_ms;
    bool draining;            /* backlog existed when we started sending */
    espos_sk_delta_stats_t st;
};

espos_sk_delta_t *espos_sk_delta_create(const espos_sk_delta_cfg_t *cfg)
{
    espos_sk_delta_t *d = calloc(1, sizeof(*d));
    if (!d) {
        return NULL;
    }
    d->cfg = *cfg;
    if (d->cfg.batch_ms == 0) {
        d->cfg.batch_ms = 100;
    }
    if (d->cfg.max_msgs == 0) {
        d->cfg.max_msgs = 1;
    }
    if (d->cfg.drain_per_s == 0) {
        d->cfg.drain_per_s = 20;
    }
    d->ring = calloc(d->cfg.max_msgs, sizeof(char *));
    if (!d->ring) {
        free(d);
        return NULL;
    }
    snprintf(d->label, sizeof(d->label), "%s", cfg->label ? cfg->label : "espos");
    return d;
}

void espos_sk_delta_destroy(espos_sk_delta_t *d)
{
    if (!d) {
        return;
    }
    for (size_t i = 0; i < d->count; i++) {
        free(d->ring[(d->head + i) % d->cfg.max_msgs]);
    }
    free(d->ring);
    free(d);
}

void espos_sk_delta_set_label(espos_sk_delta_t *d, const char *label)
{
    snprintf(d->label, sizeof(d->label), "%s", label ? label : "espos");
}

void espos_sk_delta_set_timing(espos_sk_delta_t *d, uint32_t batch_ms, uint32_t drain_per_s)
{
    if (batch_ms) {
        d->cfg.batch_ms = batch_ms;
    }
    if (drain_per_s) {
        d->cfg.drain_per_s = drain_per_s;
    }
}

/* ------------------------------------------------------------ ring */

static void ring_push(espos_sk_delta_t *d, char *msg)
{
    size_t len = strlen(msg);
    /* make room: drop oldest while over either limit */
    while (d->count > 0 && (d->count >= d->cfg.max_msgs || (d->cfg.max_bytes && d->bytes + len > d->cfg.max_bytes))) {
        char *old = d->ring[d->head];
        d->bytes -= strlen(old);
        free(old);
        d->head = (d->head + 1) % d->cfg.max_msgs;
        d->count--;
        d->st.dropped++;
    }
    if (d->cfg.max_bytes && len > d->cfg.max_bytes) {
        free(msg); /* a single message larger than the whole buffer */
        d->st.dropped++;
        return;
    }
    d->ring[(d->head + d->count) % d->cfg.max_msgs] = msg;
    d->count++;
    d->bytes += len;
}

static char *ring_pop(espos_sk_delta_t *d)
{
    if (d->count == 0) {
        return NULL;
    }
    char *m = d->ring[d->head];
    d->ring[d->head] = NULL;
    d->head = (d->head + 1) % d->cfg.max_msgs;
    d->count--;
    d->bytes -= strlen(m);
    return m;
}

void espos_sk_delta_requeue(espos_sk_delta_t *d, char *msg)
{
    if (!msg) {
        return;
    }
    if (d->count >= d->cfg.max_msgs) {
        /* full: this one is the oldest, drop it instead of a newer one */
        free(msg);
        d->st.dropped++;
        return;
    }
    d->head = (d->head + d->cfg.max_msgs - 1) % d->cfg.max_msgs;
    d->ring[d->head] = msg;
    d->count++;
    d->bytes += strlen(msg);
    d->st.taken--;
}

/* --------------------------------------------------------- pending */

esp_err_t espos_sk_delta_publish(espos_sk_delta_t *d, const char *path, const char *value_json, uint32_t now_ms)
{
    if (!d || !path || !*path || !value_json || !*value_json) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(path) >= ESPOS_SK_PATH_MAX || strlen(value_json) >= ESPOS_SK_VALUE_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    for (size_t i = 0; i < d->pending_n; i++) {
        if (strcmp(d->pending[i].path, path) == 0) {
            strcpy(d->pending[i].value, value_json); /* newest wins within the window */
            return ESP_OK;
        }
    }
    if (d->pending_n == ESPOS_SK_PENDING_MAX) {
        espos_sk_delta_flush(d, now_ms); /* window overflow: close it early */
    }
    if (d->pending_n == 0) {
        d->pending_since_ms = now_ms;
    }
    strcpy(d->pending[d->pending_n].path, path);
    strcpy(d->pending[d->pending_n].value, value_json);
    d->pending_n++;
    d->st.pending = d->pending_n;
    return ESP_OK;
}

static char *build_message(espos_sk_delta_t *d)
{
    size_t need = 96 + strlen(d->label);
    for (size_t i = 0; i < d->pending_n; i++) {
        need += 24 + strlen(d->pending[i].path) + strlen(d->pending[i].value);
    }
    char *m = malloc(need);
    if (!m) {
        return NULL;
    }
    int n = snprintf(m, need, "{\"context\":\"vessels.self\",\"updates\":[{\"source\":{\"label\":\"%s\"},\"values\":[", d->label);
    for (size_t i = 0; i < d->pending_n; i++) {
        n += snprintf(m + n, need - n, "%s{\"path\":\"%s\",\"value\":%s}", i ? "," : "", d->pending[i].path, d->pending[i].value);
    }
    snprintf(m + n, need - n, "]}]}");
    return m;
}

void espos_sk_delta_flush(espos_sk_delta_t *d, uint32_t now_ms)
{
    (void)now_ms;
    if (!d || d->pending_n == 0) {
        return;
    }
    char *m = build_message(d);
    d->pending_n = 0;
    d->st.pending = 0;
    if (m) {
        d->st.built++;
        ring_push(d, m);
    }
}

static bool batch_due(const espos_sk_delta_t *d, uint32_t now_ms)
{
    return d->pending_n > 0 && (int32_t)(now_ms - (d->pending_since_ms + d->cfg.batch_ms)) >= 0;
}

char *espos_sk_delta_take(espos_sk_delta_t *d, uint32_t now_ms, bool connected)
{
    if (!d) {
        return NULL;
    }
    if (batch_due(d, now_ms)) {
        espos_sk_delta_flush(d, now_ms);
    }
    if (!connected || d->count == 0) {
        d->draining = false;
        return NULL;
    }
    /* Backlog present (more than the message we just built): rate-limit. */
    if (d->count > 1 || d->draining) {
        uint32_t gap = 1000 / d->cfg.drain_per_s;
        if (d->st.taken && (int32_t)(now_ms - (d->last_take_ms + gap)) < 0) {
            return NULL;
        }
        d->draining = d->count > 1;
    }
    char *m = ring_pop(d);
    d->last_take_ms = now_ms;
    d->st.taken++;
    d->st.buffered = d->count;
    d->st.buffered_bytes = d->bytes;
    return m;
}

uint32_t espos_sk_delta_next_due_ms(const espos_sk_delta_t *d, uint32_t now_ms, bool connected)
{
    uint32_t due = UINT32_MAX;
    if (d->pending_n > 0) {
        int32_t left = (int32_t)((d->pending_since_ms + d->cfg.batch_ms) - now_ms);
        due = left > 0 ? (uint32_t)left : 0;
    }
    if (connected && d->count > 0) {
        uint32_t gap = 1000 / d->cfg.drain_per_s;
        int32_t left = (int32_t)((d->last_take_ms + gap) - now_ms);
        uint32_t w = (d->count > 1 || d->draining) && d->st.taken && left > 0 ? (uint32_t)left : 0;
        if (w < due) {
            due = w;
        }
    }
    return due;
}

void espos_sk_delta_stats(const espos_sk_delta_t *d, espos_sk_delta_stats_t *out)
{
    *out = d->st;
    out->pending = d->pending_n;
    out->buffered = d->count;
    out->buffered_bytes = d->bytes;
}

/* --------------------------------------------------------- helpers */

int espos_sk_json_number(char *buf, size_t size, double v)
{
    if (isnan(v) || isinf(v)) {
        return snprintf(buf, size, "null");
    }
    /* Values that are exactly a float (sensor floats promoted to double)
     * get the shortest float representation ("0.1", not 0.100000001490116);
     * genuine doubles get up to 15 significant digits. */
    bool is_float = (double)(float)v == v;
    for (int prec = 6; prec <= 15; prec++) {
        int n = snprintf(buf, size, "%.*g", prec, v);
        if (is_float ? (strtof(buf, NULL) == (float)v) : (strtod(buf, NULL) == v)) {
            return n;
        }
        if (prec == 15) {
            return n;
        }
    }
    return 0;
}

int espos_sk_json_string(char *buf, size_t size, const char *s)
{
    size_t o = 0;
    if (size < 3) {
        return 0;
    }
    buf[o++] = '"';
    for (; *s && o + 7 < size; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            buf[o++] = '\\';
            buf[o++] = (char)c;
        } else if (c < 0x20) {
            o += (size_t)snprintf(buf + o, size - o, "\\u%04x", c);
        } else {
            buf[o++] = (char)c;
        }
    }
    buf[o++] = '"';
    buf[o] = '\0';
    return (int)o;
}
