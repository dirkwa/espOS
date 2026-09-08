/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * JSON export/import and the merged JSON Schema for espos_config, using cJSON.
 */
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "espos_config.h"
#include "espos_config_priv.h"

static const char *TAG = "espos_config";

/* Shortest decimal that round-trips to the same float, parsed back as a
 * double, so cJSON prints "0.1" rather than the double expansion of the float
 * bit pattern (0.10000000149011612) or a padded "0.100000001". */
static double float_for_json(float f)
{
    char buf[32];
    for (int prec = 6; prec <= 9; prec++) {
        snprintf(buf, sizeof(buf), "%.*g", prec, (double)f);
        if (strtof(buf, NULL) == f) {
            break;
        }
    }
    return strtod(buf, NULL);
}

static cJSON *value_to_json(const espos_cfg_key_t *key, const espos_cfg_value_t *v, bool is_set,
                            bool include_secrets)
{
    switch (key->type) {
    case ESPOS_CFG_TYPE_BOOL:
        return cJSON_CreateBool(v->v.b);
    case ESPOS_CFG_TYPE_INT:
        return cJSON_CreateNumber((double)v->v.i);
    case ESPOS_CFG_TYPE_FLOAT:
        return cJSON_CreateNumber(float_for_json(v->v.f));
    case ESPOS_CFG_TYPE_STRING:
        if ((key->flags & ESPOS_CFG_FLAG_SECRET) && !include_secrets && v->v.s[0] != '\0') {
            return cJSON_CreateString(ESPOS_CONFIG_SECRET_SENTINEL);
        }
        return cJSON_CreateString(v->v.s);
    case ESPOS_CFG_TYPE_BLOB: {
        if ((key->flags & ESPOS_CFG_FLAG_SECRET) && !include_secrets && v->v.blob.len > 0) {
            return cJSON_CreateString(ESPOS_CONFIG_SECRET_SENTINEL);
        }
        size_t need = espos_b64_encoded_len(v->v.blob.len);
        char *b64 = malloc(need);
        if (!b64) {
            return NULL;
        }
        espos_b64_encode(v->v.blob.p, v->v.blob.len, b64, need);
        cJSON *j = cJSON_CreateString(b64);
        free(b64);
        return j;
    }
    }
    (void)is_set;
    return NULL;
}

static cJSON *export_ns(const espos_cfg_ns_t *nd, bool include_secrets)
{
    cJSON *obj = cJSON_CreateObject();
    if (!obj) {
        return NULL;
    }
    /* Size scratch buffers for the largest string/blob in this namespace. */
    size_t smax = 0, bmax = 0;
    for (size_t i = 0; i < nd->key_count; i++) {
        const espos_cfg_key_t *k = &nd->keys[i];
        if (k->type == ESPOS_CFG_TYPE_STRING && k->max_len + 1 > smax) {
            smax = k->max_len + 1;
        } else if (k->type == ESPOS_CFG_TYPE_BLOB && k->max_len > bmax) {
            bmax = k->max_len;
        }
    }
    char *sbuf = smax ? malloc(smax) : NULL;
    uint8_t *bbuf = bmax ? malloc(bmax) : NULL;
    if ((smax && !sbuf) || (bmax && !bbuf)) {
        free(sbuf);
        free(bbuf);
        cJSON_Delete(obj);
        return NULL;
    }
    for (size_t i = 0; i < nd->key_count; i++) {
        const espos_cfg_key_t *k = &nd->keys[i];
        espos_cfg_value_t v;
        size_t blen = 0;
        bool is_set = false;
        if (espos_config_read_effective(nd, k, &v, sbuf, bbuf, &blen, &is_set) != ESP_OK) {
            continue;
        }
        cJSON *jv = value_to_json(k, &v, is_set, include_secrets);
        if (!jv || !cJSON_AddItemToObject(obj, k->name, jv)) {
            cJSON_Delete(jv);
            free(sbuf);
            free(bbuf);
            cJSON_Delete(obj);
            return NULL;
        }
    }
    free(sbuf);
    free(bbuf);
    return obj;
}

esp_err_t espos_config_export_json(const char *only_ns, bool include_secrets, char **out_json)
{
    if (!out_json) {
        return ESP_ERR_INVALID_ARG;
    }
    *out_json = NULL;
    if (!espos_config_is_inited()) {
        return ESP_ERR_INVALID_STATE;
    }
    const espos_cfg_ns_t *single = NULL;
    if (only_ns) {
        single = espos_config_find_ns(only_ns);
        if (!single) {
            return ESP_ERR_NOT_FOUND;
        }
    }
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return ESP_ERR_NO_MEM;
    }
    /* One consistent snapshot: hold the store lock across all namespaces.
     * The iteration covers the runtime table too, so a node's settings export
     * and import exactly like a compiled namespace's. */
    espos_config_lock();
    size_t total = espos_config_ns_total_locked();
    for (size_t i = 0; i < total; i++) {
        const espos_cfg_ns_t *nd = espos_config_ns_at_locked(i);
        if (!nd || (single && nd != single)) {
            continue;
        }
        cJSON *o = export_ns(nd, include_secrets);
        if (!o || !cJSON_AddItemToObject(root, nd->name, o)) {
            espos_config_unlock();
            cJSON_Delete(o);
            cJSON_Delete(root);
            return ESP_ERR_NO_MEM;
        }
    }
    espos_config_unlock();
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    *out_json = txt;
    return ESP_OK;
}

/* ------------------------------------------------------------------ import */

static void set_error(espos_config_import_result_t *r, const char *ns, const char *key, const char *msg)
{
    if (!r) {
        return;
    }
    if (key) {
        snprintf(r->error_path, sizeof(r->error_path), "%s.%s", ns, key);
    } else {
        snprintf(r->error_path, sizeof(r->error_path), "%s", ns ? ns : "");
    }
    snprintf(r->error_msg, sizeof(r->error_msg), "%s", msg);
}

static char *make_report(bool ok, const espos_config_import_result_t *r,
                         const espos_config_plan_entry_t *plan, const size_t *changed_idx, size_t nchanged)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return NULL;
    }
    if (ok) {
        cJSON *arr = cJSON_AddArrayToObject(root, "changed");
        for (size_t i = 0; arr && i < nchanged; i++) {
            char path[40];
            snprintf(path, sizeof(path), "%s.%s", plan[changed_idx[i]].ns->name, plan[changed_idx[i]].key->name);
            cJSON_AddItemToArray(arr, cJSON_CreateString(path));
        }
        cJSON_AddBoolToObject(root, "restart_required", r->restart_required);
    } else {
        cJSON_AddStringToObject(root, "error", "validation");
        cJSON_AddStringToObject(root, "path", r->error_path);
        cJSON_AddStringToObject(root, "message", r->error_msg);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return txt;
}

/* Decode one JSON value into a plan entry. Blob bytes are malloc'ed into
 * *blob_store (caller frees). Returns false with msg on failure. */
static bool decode_value(const espos_cfg_key_t *k, const cJSON *jv, espos_cfg_value_t *out,
                         uint8_t **blob_store, char *msg, size_t msg_size)
{
    memset(out, 0, sizeof(*out));
    out->type = k->type;
    switch (k->type) {
    case ESPOS_CFG_TYPE_BOOL:
        if (!cJSON_IsBool(jv)) {
            snprintf(msg, msg_size, "expected boolean");
            return false;
        }
        out->v.b = cJSON_IsTrue(jv);
        break;
    case ESPOS_CFG_TYPE_INT: {
        if (!cJSON_IsNumber(jv)) {
            snprintf(msg, msg_size, "expected integer");
            return false;
        }
        double d = cJSON_GetNumberValue(jv);
        /* range check first (NaN fails both comparisons), then integrality */
        if (!(d >= -2147483648.0 && d <= 2147483647.0) || d != floor(d)) {
            snprintf(msg, msg_size, "expected 32-bit integer");
            return false;
        }
        out->v.i = (int32_t)d;
        break;
    }
    case ESPOS_CFG_TYPE_FLOAT:
        if (!cJSON_IsNumber(jv)) {
            snprintf(msg, msg_size, "expected number");
            return false;
        }
        out->v.f = (float)cJSON_GetNumberValue(jv);
        break;
    case ESPOS_CFG_TYPE_STRING:
        if (!cJSON_IsString(jv) || !jv->valuestring) {
            snprintf(msg, msg_size, "expected string");
            return false;
        }
        out->v.s = jv->valuestring;
        break;
    case ESPOS_CFG_TYPE_BLOB: {
        if (!cJSON_IsString(jv) || !jv->valuestring) {
            snprintf(msg, msg_size, "expected base64 string");
            return false;
        }
        size_t in_len = strlen(jv->valuestring);
        size_t cap = (in_len / 4 + 1) * 3;
        uint8_t *buf = malloc(cap ? cap : 1);
        if (!buf) {
            snprintf(msg, msg_size, "out of memory");
            return false;
        }
        size_t n = 0;
        if (espos_b64_decode(jv->valuestring, in_len, buf, cap, &n) != ESP_OK) {
            free(buf);
            snprintf(msg, msg_size, "invalid base64");
            return false;
        }
        *blob_store = buf;
        out->v.blob.p = buf;
        out->v.blob.len = n;
        break;
    }
    }
    return espos_config_validate(k, out, msg, msg_size);
}

/* JSON may carry U+0000 (raw, or as the \u0000 escape) but our values and
 * key names are C strings; refuse rather than silently truncate. Scans only
 * the parsed span so a NUL after the document stays tolerated. */
static bool json_has_nul(const char *p, const char *end)
{
    if (memchr(p, 0, (size_t)(end - p))) {
        return true;
    }
    for (; p + 1 < end; p++) {
        if (*p != '\\') {
            continue;
        }
        if (p[1] == 'u' && p + 5 < end && p[2] == '0' && p[3] == '0' && p[4] == '0' && p[5] == '0') {
            return true;
        }
        p++; /* skip the escaped char so "\\u0000" (a literal backslash) is not matched */
    }
    return false;
}

esp_err_t espos_config_import_json(const char *json, size_t json_len, bool ignore_unknown,
                                   espos_config_import_result_t *result, char **out_report_json)
{
    espos_config_import_result_t local = { 0 };
    if (!result) {
        result = &local;
    }
    memset(result, 0, sizeof(*result));
    if (out_report_json) {
        *out_report_json = NULL;
    }
    if (!json) {
        return ESP_ERR_INVALID_ARG;
    }

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &end, false);
    if (root && json_has_nul(json, end)) {
        cJSON_Delete(root);
        root = NULL;
    }
    if (root) {
        /* Only whitespace may follow the document. */
        for (const char *p = end; p < json + json_len; p++) {
            if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '\0') {
                cJSON_Delete(root);
                root = NULL;
                break;
            }
        }
    }
    if (!root) {
        set_error(result, "", NULL, "malformed JSON");
        if (out_report_json) {
            *out_report_json = make_report(false, result, NULL, NULL, 0);
        }
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    espos_config_plan_entry_t *plan = NULL;
    uint8_t **blobs = NULL;
    size_t *changed_idx = NULL;
    size_t nplan = 0;
    size_t leaves = 0;

    if (!cJSON_IsObject(root)) {
        set_error(result, "", NULL, "expected object of namespaces");
        err = ESP_ERR_INVALID_ARG;
        goto out;
    }

    /* Upper bound on plan size: every JSON leaf. */
    for (cJSON *jns = root->child; jns; jns = jns->next) {
        for (cJSON *jk = jns->child; cJSON_IsObject(jns) && jk; jk = jk->next) {
            leaves++;
        }
    }
    plan = calloc(leaves ? leaves : 1, sizeof(*plan));
    blobs = calloc(leaves ? leaves : 1, sizeof(*blobs));
    changed_idx = calloc(leaves ? leaves : 1, sizeof(*changed_idx));
    if (!plan || !blobs || !changed_idx) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Pass 1: validate everything, build the plan. Nothing is written yet. */
    for (cJSON *jns = root->child; jns; jns = jns->next) {
        const char *nsname = jns->string ? jns->string : "";
        const espos_cfg_ns_t *nd = espos_config_find_ns(nsname);
        if (!nd) {
            if (ignore_unknown) {
                continue;
            }
            set_error(result, nsname, NULL, "unknown namespace");
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        if (!cJSON_IsObject(jns)) {
            set_error(result, nsname, NULL, "expected object of keys");
            err = ESP_ERR_INVALID_ARG;
            goto out;
        }
        for (cJSON *jk = jns->child; jk; jk = jk->next) {
            const char *kname = jk->string ? jk->string : "";
            const espos_cfg_key_t *kd = espos_config_find_key(nd, kname);
            if (!kd) {
                if (ignore_unknown) {
                    continue;
                }
                set_error(result, nsname, kname, "unknown key");
                err = ESP_ERR_INVALID_ARG;
                goto out;
            }
            /* Duplicate keys in one document: last one wins (cJSON keeps both). */
            for (size_t i = 0; i < nplan; i++) {
                if (plan[i].ns == nd && plan[i].key == kd) {
                    free(blobs[i]);
                    blobs[i] = NULL;
                    memmove(&plan[i], &plan[i + 1], (nplan - i - 1) * sizeof(*plan));
                    memmove(&blobs[i], &blobs[i + 1], (nplan - i - 1) * sizeof(*blobs));
                    nplan--;
                    blobs[nplan] = NULL;
                    memset(&plan[nplan], 0, sizeof(*plan));
                    break;
                }
            }
            if (kd->flags & ESPOS_CFG_FLAG_READ_ONLY) {
                /* Left untouched, like a secret echoed back as the sentinel.
                 * Rejecting instead would break the ordinary round trip: an
                 * export contains every key, and re-importing that file is
                 * exactly what "restore my settings" does. */
                continue;
            }
            espos_config_plan_entry_t *e = &plan[nplan];
            e->ns = nd;
            e->key = kd;
            if (cJSON_IsNull(jk)) {
                e->reset = true;
                nplan++;
                continue;
            }
            if ((kd->flags & ESPOS_CFG_FLAG_SECRET) && cJSON_IsString(jk) && jk->valuestring &&
                strcmp(jk->valuestring, ESPOS_CONFIG_SECRET_SENTINEL) == 0) {
                continue; /* redacted value echoed back: leave untouched */
            }
            char msg[80];
            if (!decode_value(kd, jk, &e->val, &blobs[nplan], msg, sizeof(msg))) {
                set_error(result, nsname, kname, msg);
                err = ESP_ERR_INVALID_ARG;
                goto out;
            }
            nplan++;
        }
    }

    /* Pass 2: apply under one lock. */
    {
        size_t nchanged = 0;
        bool restart = false;
        err = espos_config_apply_plan(plan, nplan, changed_idx, &nchanged, &restart);
        result->changed = nchanged;
        result->restart_required = restart;
        if (err != ESP_OK) {
            set_error(result, "", NULL, "storage write failed");
            ESP_LOGE(TAG, "import: partial write failure (%s)", esp_err_to_name(err));
        } else if (out_report_json) {
            *out_report_json = make_report(true, result, plan, changed_idx, nchanged);
        }
    }

out:
    if (err == ESP_ERR_INVALID_ARG && out_report_json && !*out_report_json) {
        *out_report_json = make_report(false, result, NULL, NULL, 0);
    }
    if (blobs) {
        for (size_t i = 0; i < leaves; i++) {
            free(blobs[i]);
        }
    }
    free(blobs);
    free(plan);
    free(changed_idx);
    cJSON_Delete(root);
    return err;
}

/* ------------------------------------------------------------------ schema */

/* One key's JSON Schema property, the runtime twin of build_schema() in
 * tools/espos_gen_config.py. The two must agree: the UI has one renderer, and
 * a node's multiplier field has to look exactly like a compiled one. */
static cJSON *key_to_schema(const espos_cfg_key_t *k)
{
    cJSON *p = cJSON_CreateObject();
    if (!p) {
        return NULL;
    }
    cJSON_AddStringToObject(p, "title", k->title ? k->title : k->name);
    if (k->description && k->description[0]) {
        cJSON_AddStringToObject(p, "description", k->description);
    }
    switch (k->type) {
    case ESPOS_CFG_TYPE_BOOL:
        cJSON_AddStringToObject(p, "type", "boolean");
        cJSON_AddBoolToObject(p, "default", k->def.b);
        break;
    case ESPOS_CFG_TYPE_INT:
        cJSON_AddStringToObject(p, "type", "integer");
        cJSON_AddNumberToObject(p, "default", (double)k->def.i);
        if (k->min.i != INT32_MIN) {
            cJSON_AddNumberToObject(p, "minimum", (double)k->min.i);
        }
        if (k->max.i != INT32_MAX) {
            cJSON_AddNumberToObject(p, "maximum", (double)k->max.i);
        }
        break;
    case ESPOS_CFG_TYPE_FLOAT:
        cJSON_AddStringToObject(p, "type", "number");
        cJSON_AddNumberToObject(p, "default", float_for_json(k->def.f));
        if (k->has_min) {
            cJSON_AddNumberToObject(p, "minimum", float_for_json(k->min.f));
        }
        if (k->has_max) {
            cJSON_AddNumberToObject(p, "maximum", float_for_json(k->max.f));
        }
        break;
    case ESPOS_CFG_TYPE_STRING:
        cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "default", k->def.s ? k->def.s : "");
        cJSON_AddNumberToObject(p, "maxLength", (double)k->max_len);
        if (k->enum_values && k->enum_count) {
            cJSON *e = cJSON_AddArrayToObject(p, "enum");
            for (size_t i = 0; e && i < k->enum_count; i++) {
                cJSON_AddItemToArray(e, cJSON_CreateString(k->enum_values[i]));
            }
        }
        break;
    case ESPOS_CFG_TYPE_BLOB:
        cJSON_AddStringToObject(p, "type", "string");
        cJSON_AddStringToObject(p, "contentEncoding", "base64");
        cJSON_AddStringToObject(p, "x-espos-type", "blob");
        cJSON_AddNumberToObject(p, "x-espos-maxBytes", (double)k->max_len);
        cJSON_AddStringToObject(p, "default", "");
        break;
    }
    if (k->unit && k->unit[0]) {
        cJSON_AddStringToObject(p, "x-espos-unit", k->unit);
    }
    if (k->flags & ESPOS_CFG_FLAG_SECRET) {
        cJSON_AddBoolToObject(p, "writeOnly", true);
        cJSON_AddStringToObject(p, "format", "password");
        cJSON_AddBoolToObject(p, "x-espos-secret", true);
    }
    if (k->flags & ESPOS_CFG_FLAG_RESTART_REQUIRED) {
        cJSON_AddBoolToObject(p, "x-espos-restartRequired", true);
    }
    if (k->flags & ESPOS_CFG_FLAG_READ_ONLY) {
        cJSON_AddBoolToObject(p, "readOnly", true);
    }
    if (k->display.group && k->display.group[0]) {
        cJSON_AddStringToObject(p, "x-espos-group", k->display.group);
    }
    if (k->display.display_mul != 0.0f && k->display.display_mul != 1.0f) {
        cJSON_AddNumberToObject(p, "x-espos-displayMultiplier", float_for_json(k->display.display_mul));
    }
    if (k->display.display_off != 0.0f) {
        cJSON_AddNumberToObject(p, "x-espos-displayOffset", float_for_json(k->display.display_off));
    }
    if (k->display.table_columns && k->display.table_column_count) {
        cJSON_AddStringToObject(p, "x-espos-format", "table");
        cJSON *c = cJSON_AddArrayToObject(p, "x-espos-columns");
        for (size_t i = 0; c && i < k->display.table_column_count; i++) {
            cJSON_AddItemToArray(c, cJSON_CreateString(k->display.table_columns[i]));
        }
    }
    return p;
}

static cJSON *ns_to_schema(const espos_cfg_ns_t *nd)
{
    cJSON *o = cJSON_CreateObject();
    if (!o) {
        return NULL;
    }
    cJSON_AddStringToObject(o, "type", "object");
    cJSON_AddStringToObject(o, "title", nd->title ? nd->title : nd->name);
    if (nd->description && nd->description[0]) {
        cJSON_AddStringToObject(o, "description", nd->description);
    }
    cJSON_AddNumberToObject(o, "x-espos-version", (double)nd->version);
    /* Marks the section as one a node put there: the UI can offer to forget
     * its stored values when the node is gone, which it must never offer for
     * a built-in. */
    cJSON_AddBoolToObject(o, "x-espos-runtime", true);
    cJSON *props = cJSON_AddObjectToObject(o, "properties");
    if (!props) {
        cJSON_Delete(o);
        return NULL;
    }
    for (size_t i = 0; i < nd->key_count; i++) {
        cJSON *p = key_to_schema(&nd->keys[i]);
        if (!p || !cJSON_AddItemToObject(props, nd->keys[i].name, p)) {
            cJSON_Delete(p);
            cJSON_Delete(o);
            return NULL;
        }
    }
    cJSON_AddBoolToObject(o, "additionalProperties", false);
    return o;
}

/* Compiled etag, hashed together with the generation counter. The merged
 * document is not rehashed: what a client needs is a value that changes
 * whenever the schema could have changed, and register/unregister is the only
 * way a runtime namespace appears or goes.
 *
 * With nothing registered the document IS the compiled one, so the compiled
 * ETag is served unchanged and a browser that cached it keeps its 304 —
 * including on a device where nodes were registered and then all removed
 * again, which is exactly the state the compiled schema describes. */
static void schema_etag(char out[ESPOS_CFG_ETAG_MAX])
{
    if (espos_config_runtime_ns_count() == 0) {
        snprintf(out, ESPOS_CFG_ETAG_MAX, "%s", espos_cfg_schema_etag);
        return;
    }
    uint32_t h = 5381;
    for (const char *p = espos_cfg_schema_etag; *p; p++) {
        h = h * 33u + (unsigned char)*p;
    }
    h = h * 33u + espos_config_generation();
    snprintf(out, ESPOS_CFG_ETAG_MAX, "%.8s-%08" PRIx32, espos_cfg_schema_etag, h);
}

void espos_config_schema_etag(char etag[ESPOS_CFG_ETAG_MAX])
{
    if (etag) {
        schema_etag(etag);
    }
}

esp_err_t espos_config_schema_json(char **out, char etag[ESPOS_CFG_ETAG_MAX])
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    if (etag) {
        schema_etag(etag);
    }
    /* Snapshot the runtime namespaces under the lock, then build outside it:
     * cJSON allocates, and holding the store lock across that would block
     * every getter for the length of a document build. The descriptors stay
     * valid because unregistering one is the caller's promise that nobody is
     * still using it. */
    espos_config_lock();
    size_t total = espos_config_ns_total_locked();
    size_t nrt = total > espos_cfg_namespace_count ? total - espos_cfg_namespace_count : 0;
    const espos_cfg_ns_t **rt = nrt ? calloc(nrt, sizeof(*rt)) : NULL;
    if (nrt && !rt) {
        espos_config_unlock();
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < nrt; i++) {
        rt[i] = espos_config_ns_at_locked(espos_cfg_namespace_count + i);
    }
    espos_config_unlock();

    if (nrt == 0) {
        /* The overwhelmingly common case: hand back the compiled text. */
        free(rt);
        char *copy = malloc(espos_cfg_schema_json_len + 1);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, espos_cfg_schema_json, espos_cfg_schema_json_len + 1);
        *out = copy;
        return ESP_OK;
    }

    cJSON *root = cJSON_ParseWithLength(espos_cfg_schema_json, espos_cfg_schema_json_len);
    cJSON *props = root ? cJSON_GetObjectItemCaseSensitive(root, "properties") : NULL;
    if (!props || !cJSON_IsObject(props)) {
        cJSON_Delete(root);
        free(rt);
        ESP_LOGE(TAG, "compiled schema is not usable as a merge base");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < nrt; i++) {
        if (!rt[i]) {
            continue; /* unregistered between the snapshot and here */
        }
        cJSON *o = ns_to_schema(rt[i]);
        if (!o || !cJSON_AddItemToObject(props, rt[i]->name, o)) {
            cJSON_Delete(o);
            err = ESP_ERR_NO_MEM;
            break;
        }
    }
    free(rt);
    char *txt = err == ESP_OK ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (err != ESP_OK) {
        return err;
    }
    if (!txt) {
        return ESP_ERR_NO_MEM;
    }
    *out = txt;
    return ESP_OK;
}
