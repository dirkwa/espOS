/*
 * SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * JSON export/import for espos_config, using cJSON.
 */
#include <math.h>
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
    /* One consistent snapshot: hold the store lock across all namespaces. */
    espos_config_lock();
    for (size_t i = 0; i < espos_cfg_namespace_count; i++) {
        const espos_cfg_ns_t *nd = &espos_cfg_namespaces[i];
        if (single && nd != single) {
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
