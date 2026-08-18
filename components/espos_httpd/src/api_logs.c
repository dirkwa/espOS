/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * /api/v1/logs — the log ring, paged by sequence number, streamed in
 * pages so a large ring never has to be materialised in RAM; the ring
 * lock is only held while copying one page.
 *
 * /api/v1/logs/level — esp_log_level_set() over HTTP.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "espos_httpd.h"
#include "espos_httpd_priv.h"
#include "espos_httpd_sse.h"
#include "espos_log.h"

#define PAGE_LINES   16
#define LIMIT_DEFAULT 200
#define LIMIT_MAX     1000

typedef struct {
    char *lines[PAGE_LINES];
    uint32_t seq[PAGE_LINES];
    size_t n;
} page_t;

static bool collect(uint32_t seq, const char *line, size_t len, void *arg)
{
    page_t *p = arg;
    char *copy = malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, line, len);
    copy[len] = '\0';
    p->lines[p->n] = copy;
    p->seq[p->n] = seq;
    p->n++;
    return p->n < PAGE_LINES;
}

/* JSON string escape into a malloc'ed buffer (quotes included). */
static char *json_quote(const char *in)
{
    size_t n = 2;
    for (const char *c = in; *c; c++) {
        unsigned char u = (unsigned char)*c;
        n += (u == '"' || u == '\\') ? 2 : (u < 0x20) ? 6 : 1;
    }
    char *out = malloc(n + 1);
    if (!out) {
        return NULL;
    }
    char *o = out;
    *o++ = '"';
    for (const char *c = in; *c; c++) {
        unsigned char u = (unsigned char)*c;
        if (u == '"' || u == '\\') {
            *o++ = '\\';
            *o++ = (char)u;
        } else if (u == '\n') {
            *o++ = '\\'; *o++ = 'n';
        } else if (u == '\t') {
            *o++ = '\\'; *o++ = 't';
        } else if (u < 0x20) {
            o += sprintf(o, "\\u%04x", u);
        } else {
            *o++ = (char)u;
        }
    }
    *o++ = '"';
    *o = '\0';
    return out;
}

static uint32_t query_u32(httpd_req_t *req, const char *key, uint32_t dflt)
{
    char q[96], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        return dflt;
    }
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return dflt;
    }
    return (uint32_t)strtoul(v, NULL, 10);
}

static esp_err_t logs_get(httpd_req_t *req)
{
    uint32_t after = query_u32(req, "after", 0);
    uint32_t limit = query_u32(req, "limit", LIMIT_DEFAULT);
    if (limit == 0 || limit > LIMIT_MAX) {
        limit = LIMIT_MAX;
    }
    espos_log_stats_t st;
    espos_log_stats(&st);
    /* "after" older than what we still have: the client missed lines. */
    bool gap = (int32_t)(after + 1 - st.first) < 0;
    if (gap) {
        after = st.first - 1;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    char head[160];
    int hn = snprintf(head, sizeof(head), "{\"first\":%u,\"next\":%u,\"dropped\":%u,\"size\":%u,\"used\":%u,\"gap\":%s,\"from\":%u,\"lines\":[",
                      (unsigned)st.first, (unsigned)st.next, (unsigned)st.dropped, (unsigned)st.size, (unsigned)st.used,
                      gap ? "true" : "false", (unsigned)(after + 1));
    esp_err_t err = httpd_resp_send_chunk(req, head, hn);
    uint32_t sent = 0;
    bool first = true;
    while (err == ESP_OK && sent < limit) {
        page_t page = { 0 };
        size_t want = limit - sent < PAGE_LINES ? limit - sent : PAGE_LINES;
        espos_log_read(after, want, collect, &page);
        if (page.n == 0) {
            break;
        }
        for (size_t i = 0; i < page.n && err == ESP_OK; i++) {
            char *q = json_quote(page.lines[i]);
            if (!q) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            if (!first) {
                err = httpd_resp_send_chunk(req, ",", 1);
            }
            first = false;
            if (err == ESP_OK) {
                err = httpd_resp_send_chunk(req, q, strlen(q));
            }
            free(q);
        }
        after = page.seq[page.n - 1];
        sent += page.n;
        for (size_t i = 0; i < page.n; i++) {
            free(page.lines[i]);
        }
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, "]}", 2);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    } else {
        httpd_resp_send_chunk(req, NULL, 0);
    }
    return err == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t level_put(httpd_req_t *req)
{
    if (!espos_httpd_require_json(req)) {
        return ESP_OK;
    }
    char *body = NULL;
    size_t len = 0;
    if (espos_httpd_read_body(req, &body, &len) != ESP_OK) {
        return ESP_FAIL;
    }
    cJSON *j = cJSON_ParseWithLength(body, len);
    free(body);
    const cJSON *tag = j ? cJSON_GetObjectItem(j, "tag") : NULL;
    const cJSON *level = j ? cJSON_GetObjectItem(j, "level") : NULL;
    const char *t = cJSON_IsString(tag) && tag->valuestring[0] ? tag->valuestring : "*";
    if (!cJSON_IsString(level) || espos_log_set_level(t, level->valuestring) != ESP_OK) {
        cJSON_Delete(j);
        return espos_httpd_send_error(req, "400 Bad Request", "validation",
                                      "expected {\"level\": none|error|warn|info|debug|verbose[, \"tag\": \"...\"]}");
    }
    char out[96];
    snprintf(out, sizeof(out), "{\"tag\":\"%s\",\"level\":\"%s\"}", t, espos_log_level_name(esp_log_level_get(t)));
    cJSON_Delete(j);
    return espos_httpd_send_json(req, "200 OK", out);
}

static void notify(uint32_t next, void *arg)
{
    (void)arg;
    char ev[32];
    snprintf(ev, sizeof(ev), "{\"next\":%u}", (unsigned)next);
    espos_httpd_sse_publish("logs", ev);
}

esp_err_t espos_httpd_register_logs_api(httpd_handle_t h)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/api/v1/logs", .method = HTTP_GET, .handler = logs_get },
        { .uri = "/api/v1/logs/level", .method = HTTP_PUT, .handler = level_put },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        esp_err_t err = httpd_register_uri_handler(h, &uris[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    espos_log_set_notify(notify, NULL);
    return ESP_OK;
}
