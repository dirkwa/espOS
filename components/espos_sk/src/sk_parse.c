/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "espos_sk_parse.h"

/* Printed JSON strings we hand out live here until espos_sk_frame_free. */
typedef struct {
    cJSON *root;
    char **strings;
    size_t n, cap;
} arena_t;

static const char *keep(arena_t *a, char *s)
{
    if (!s) {
        return NULL;
    }
    if (a->n == a->cap) {
        size_t nc = a->cap ? a->cap * 2 : 16;
        char **ns = realloc(a->strings, nc * sizeof(char *));
        if (!ns) {
            free(s);
            return NULL;
        }
        a->strings = ns;
        a->cap = nc;
    }
    a->strings[a->n++] = s;
    return s;
}

static const char *str_of(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

bool espos_sk_path_matches(const char *pattern, const char *path)
{
    size_t pl = strlen(pattern);
    if (pl == 0) {
        return false;
    }
    if (pattern[pl - 1] == '*') {
        size_t n = pl - 1;
        if (n > 0 && pattern[n - 1] == '.') {
            /* "a.b.*" matches "a.b" and "a.b.c" */
            if (strncmp(pattern, path, n - 1) == 0 && (path[n - 1] == '\0' || path[n - 1] == '.')) {
                return true;
            }
            return false;
        }
        return strncmp(pattern, path, n) == 0;
    }
    return strcmp(pattern, path) == 0;
}

size_t espos_sk_frame_parse(const char *json, size_t len, espos_sk_frame_t *info, espos_sk_update_cb_t cb, void *arg)
{
    memset(info, 0, sizeof(*info));
    cJSON *root = cJSON_ParseWithLength(json, len);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return 0;
    }
    arena_t *a = calloc(1, sizeof(*a));
    if (!a) {
        cJSON_Delete(root);
        return 0;
    }
    a->root = root;
    info->_priv = a;
    /* kind detection */
    const char *err = str_of(root, "errorMessage");
    const cJSON *put = cJSON_GetObjectItem(root, "put");
    if (err) {
        info->kind = ESPOS_SK_FRAME_ERROR;
        info->error = err;
    } else if (put && (cJSON_IsArray(put) || cJSON_IsObject(put))) {
        /* Checked before RESPONSE: a PUT request also carries a requestId,
         * and it is the presence of "put" that distinguishes the server
         * asking us to do something from the server telling us how our own
         * request went. A response never carries "put". */
        info->kind = ESPOS_SK_FRAME_PUT;
        info->request_id = str_of(root, "requestId");
    } else if (str_of(root, "requestId") && (str_of(root, "state") || cJSON_HasObjectItem(root, "statusCode"))) {
        info->kind = ESPOS_SK_FRAME_RESPONSE;
        info->request_id = str_of(root, "requestId");
        info->state = str_of(root, "state");
        const cJSON *sc = cJSON_GetObjectItem(root, "statusCode");
        info->status_code = cJSON_IsNumber(sc) ? sc->valueint : 0;
        info->message = str_of(root, "message");
    } else if (cJSON_IsArray(cJSON_GetObjectItem(root, "updates"))) {
        info->kind = ESPOS_SK_FRAME_DELTA;
    } else if (str_of(root, "self") || str_of(root, "version")) {
        info->kind = ESPOS_SK_FRAME_HELLO;
        info->self = str_of(root, "self");
    }

    size_t delivered = 0;
    if (info->kind == ESPOS_SK_FRAME_PUT && cb) {
        const char *ctx = str_of(root, "context");
        /* Two shapes, one loop. signalk-server writes an array; a client
         * (espos_sk_put) writes a single object. Wrapping the object in a
         * one-element iteration keeps the handler side unaware of which. */
        const cJSON *one = cJSON_IsObject(put) ? put : NULL;
        const cJSON *item = one ? one : (cJSON_IsArray(put) ? put->child : NULL);
        for (; item; item = one ? NULL : item->next) {
            const char *path = str_of(item, "path");
            const cJSON *val = cJSON_GetObjectItem(item, "value");
            /* A PUT with no value is meaningless, but an explicit JSON null
             * is not: it is how a switch is cleared. cJSON gives a real node
             * for null, so only a missing key is rejected. */
            if (!path || !val) {
                continue;
            }
            espos_sk_update_t u = { .context = ctx, .path = path, .request_id = info->request_id };
            u.value_json = keep(a, cJSON_PrintUnformatted(val));
            if (!u.value_json) {
                continue;
            }
            delivered++;
            if (!cb(&u, arg)) {
                break;
            }
        }
    } else if (info->kind == ESPOS_SK_FRAME_DELTA && cb) {
        const char *ctx = str_of(root, "context");
        const cJSON *upd;
        bool go = true;
        cJSON_ArrayForEach(upd, cJSON_GetObjectItem(root, "updates"))
        {
            if (!go) {
                break;
            }
            const char *ts = str_of(upd, "timestamp");
            const char *src = str_of(upd, "$source");
            if (!src) {
                const cJSON *so = cJSON_GetObjectItem(upd, "source");
                src = str_of(so, "label");
            }
            const cJSON *item;
            cJSON_ArrayForEach(item, cJSON_GetObjectItem(upd, "values"))
            {
                const char *path = str_of(item, "path");
                const cJSON *val = cJSON_GetObjectItem(item, "value");
                if (!path || !val) {
                    continue;
                }
                espos_sk_update_t u = { .context = ctx, .path = path, .timestamp = ts, .source = src };
                u.value_json = keep(a, cJSON_PrintUnformatted(val));
                if (!u.value_json) {
                    continue;
                }
                delivered++;
                if (!cb(&u, arg)) {
                    go = false;
                    break;
                }
            }
            if (!go) {
                break;
            }
            cJSON_ArrayForEach(item, cJSON_GetObjectItem(upd, "meta"))
            {
                const char *path = str_of(item, "path");
                const cJSON *val = cJSON_GetObjectItem(item, "value");
                if (!path || !cJSON_IsObject(val)) {
                    continue;
                }
                espos_sk_update_t u = { .context = ctx, .path = path, .timestamp = ts, .source = src };
                u.meta_json = keep(a, cJSON_PrintUnformatted(val));
                if (!u.meta_json) {
                    continue;
                }
                delivered++;
                if (!cb(&u, arg)) {
                    go = false;
                    break;
                }
            }
        }
    }
    return delivered;
}

void espos_sk_frame_free(espos_sk_frame_t *info)
{
    arena_t *a = info->_priv;
    if (!a) {
        return;
    }
    for (size_t i = 0; i < a->n; i++) {
        free(a->strings[i]);
    }
    free(a->strings);
    cJSON_Delete(a->root);
    free(a);
    info->_priv = NULL;
}

char *espos_sk_put_response_frame(const char *request_id, const char *state, int status_code, const char *message)
{
    if (!request_id || !*request_id || !state) {
        return NULL;
    }
    size_t n = strlen(request_id) + strlen(state) + (message ? strlen(message) : 0) + 128;
    char *frame = malloc(n);
    if (!frame) {
        return NULL;
    }
    int w = snprintf(frame, n, "{\"requestId\":\"%s\",\"state\":\"%s\",\"statusCode\":%d", request_id, state, status_code);
    if (message && *message) {
        /* Built with cJSON so a message containing a quote cannot break the
         * frame. An unescaped quote here would make the whole response
         * unparseable, and the server would then never resolve the request
         * -- the failure mode is a 60 s hang, not a visible error. */
        cJSON *m = cJSON_CreateString(message);
        char *esc = m ? cJSON_PrintUnformatted(m) : NULL;
        if (esc) {
            w += snprintf(frame + w, n - w, ",\"message\":%s", esc);
            free(esc);
        }
        cJSON_Delete(m);
    }
    snprintf(frame + w, n - w, "}");
    return frame;
}
