/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * espos_config core: descriptor lookup, validated typed access, versioning and
 * migrations, factory reset, change notification. Storage goes through the
 * injected backend so this file is host-testable.
 */
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "espos_config.h"
#include "espos_config_priv.h"

static const char *TAG = "espos_config";

#ifndef CONFIG_ESPOS_CONFIG_MAX_MIGRATIONS
#define CONFIG_ESPOS_CONFIG_MAX_MIGRATIONS 16
#endif
#ifndef CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS
#define CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS 8
#endif

typedef struct {
    const espos_cfg_ns_t *desc;
    espos_config_bh_t h;
} ns_state_t;

typedef struct {
    const char *ns;
    uint16_t from;
    espos_config_migrate_fn_t fn;
    void *arg;
} migration_t;

typedef struct {
    espos_config_change_cb_t cb;
    void *arg;
} subscriber_t;

struct espos_config_migrate_ctx {
    ns_state_t *ns;
    uint16_t from;
};

static struct {
    const espos_config_backend_t *be;
    void *be_ctx;
    ns_state_t *ns;
    size_t ns_count;
    SemaphoreHandle_t lock;
    bool inited;
    bool storage_reset;
    migration_t migrations[CONFIG_ESPOS_CONFIG_MAX_MIGRATIONS];
    size_t migration_count;
    subscriber_t subs[CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS];
} s;

/* --------------------------------------------------------------- utilities */

static void cfg_lock(void)
{
    xSemaphoreTake(s.lock, portMAX_DELAY);
}

static void cfg_unlock(void)
{
    xSemaphoreGive(s.lock);
}

const espos_cfg_ns_t *espos_config_find_ns(const char *ns)
{
    if (!ns) {
        return NULL;
    }
    for (size_t i = 0; i < espos_cfg_namespace_count; i++) {
        if (strcmp(espos_cfg_namespaces[i].name, ns) == 0) {
            return &espos_cfg_namespaces[i];
        }
    }
    return NULL;
}

const espos_cfg_key_t *espos_config_find_key(const espos_cfg_ns_t *ns, const char *key)
{
    if (!ns || !key) {
        return NULL;
    }
    for (size_t i = 0; i < ns->key_count; i++) {
        if (strcmp(ns->keys[i].name, key) == 0) {
            return &ns->keys[i];
        }
    }
    return NULL;
}

static ns_state_t *ns_state_for(const espos_cfg_ns_t *desc)
{
    return &s.ns[desc - espos_cfg_namespaces];
}

static esp_err_t lookup(const char *ns, const char *key, ns_state_t **nss, const espos_cfg_key_t **kd)
{
    const espos_cfg_ns_t *nd = espos_config_find_ns(ns);
    if (!nd) {
        return ESP_ERR_NOT_FOUND;
    }
    const espos_cfg_key_t *k = espos_config_find_key(nd, key);
    if (!k) {
        return ESP_ERR_NOT_FOUND;
    }
    *nss = ns_state_for(nd);
    *kd = k;
    return ESP_OK;
}

/* ------------------------------------------------------------- validation */

bool espos_config_validate(const espos_cfg_key_t *key, const espos_cfg_value_t *val,
                           char *msg, size_t msg_size)
{
#define FAIL(...) do { if (msg && msg_size) { snprintf(msg, msg_size, __VA_ARGS__); } return false; } while (0)
    if (val->type != key->type) {
        static const char *const names[] = { "?", "boolean", "integer", "number", "string", "blob(base64)" };
        FAIL("expected %s", names[key->type <= ESPOS_CFG_TYPE_BLOB ? key->type : 0]);
    }
    switch (key->type) {
    case ESPOS_CFG_TYPE_BOOL:
        return true;
    case ESPOS_CFG_TYPE_INT:
        if (val->v.i < key->min.i || val->v.i > key->max.i) {
            FAIL("out of range [%ld,%ld]", (long)key->min.i, (long)key->max.i);
        }
        return true;
    case ESPOS_CFG_TYPE_FLOAT:
        if (isnan(val->v.f) || isinf(val->v.f)) {
            FAIL("not a finite number");
        }
        if (key->has_min && val->v.f < key->min.f) {
            FAIL("below minimum %g", (double)key->min.f);
        }
        if (key->has_max && val->v.f > key->max.f) {
            FAIL("above maximum %g", (double)key->max.f);
        }
        return true;
    case ESPOS_CFG_TYPE_STRING: {
        if (!val->v.s) {
            FAIL("expected string");
        }
        size_t n = strlen(val->v.s);
        if (n > key->max_len) {
            FAIL("longer than %u bytes", (unsigned)key->max_len);
        }
        if (key->enum_values) {
            for (size_t i = 0; i < key->enum_count; i++) {
                if (strcmp(key->enum_values[i], val->v.s) == 0) {
                    return true;
                }
            }
            FAIL("not an allowed value");
        }
        return true;
    }
    case ESPOS_CFG_TYPE_BLOB:
        if (val->v.blob.len > key->max_len) {
            FAIL("longer than %u bytes", (unsigned)key->max_len);
        }
        if (val->v.blob.len && !val->v.blob.p) {
            FAIL("expected blob");
        }
        return true;
    }
    FAIL("unknown type");
#undef FAIL
}

/* ------------------------------------------------------------ raw storage */

/* Take/release the store lock (JSON export runs a whole snapshot under it). */
void espos_config_lock(void)
{
    cfg_lock();
}

void espos_config_unlock(void)
{
    cfg_unlock();
}

bool espos_config_is_inited(void)
{
    return s.inited;
}

/* Read the effective (stored-and-valid, else default) value. Lock held. */
esp_err_t espos_config_read_effective(const espos_cfg_ns_t *nd, const espos_cfg_key_t *key,
                                      espos_cfg_value_t *out, char *sbuf, uint8_t *bbuf,
                                      size_t *blen, bool *is_set)
{
    ns_state_t *nss = ns_state_for(nd);
    esp_err_t err;
    bool set = false;
    memset(out, 0, sizeof(*out));
    out->type = key->type;

    switch (key->type) {
    case ESPOS_CFG_TYPE_BOOL: {
        uint8_t u = 0;
        size_t len = sizeof(u);
        err = s.be->get(s.be_ctx, nss->h, key->name, key->type, &u, &len);
        if (err == ESP_OK && u <= 1) {
            out->v.b = u != 0;
            set = true;
        } else {
            out->v.b = key->def.b;
        }
        break;
    }
    case ESPOS_CFG_TYPE_INT: {
        int32_t i = 0;
        size_t len = sizeof(i);
        err = s.be->get(s.be_ctx, nss->h, key->name, key->type, &i, &len);
        if (err == ESP_OK && i >= key->min.i && i <= key->max.i) {
            out->v.i = i;
            set = true;
        } else {
            out->v.i = key->def.i;
        }
        break;
    }
    case ESPOS_CFG_TYPE_FLOAT: {
        float f = 0;
        size_t len = sizeof(f);
        err = s.be->get(s.be_ctx, nss->h, key->name, key->type, &f, &len);
        espos_cfg_value_t tmp = { .type = ESPOS_CFG_TYPE_FLOAT, .v.f = f };
        if (err == ESP_OK && espos_config_validate(key, &tmp, NULL, 0)) {
            out->v.f = f;
            set = true;
        } else {
            out->v.f = key->def.f;
        }
        break;
    }
    case ESPOS_CFG_TYPE_STRING: {
        size_t len = key->max_len + 1;
        err = s.be->get(s.be_ctx, nss->h, key->name, key->type, sbuf, &len);
        if (err == ESP_OK) {
            sbuf[key->max_len] = '\0';
            espos_cfg_value_t tmp = { .type = ESPOS_CFG_TYPE_STRING, .v.s = sbuf };
            if (espos_config_validate(key, &tmp, NULL, 0)) {
                out->v.s = sbuf;
                set = true;
                break;
            }
        }
        strncpy(sbuf, key->def.s, key->max_len + 1);
        sbuf[key->max_len] = '\0';
        out->v.s = sbuf;
        break;
    }
    case ESPOS_CFG_TYPE_BLOB: {
        size_t len = key->max_len;
        err = s.be->get(s.be_ctx, nss->h, key->name, key->type, bbuf, &len);
        if (err == ESP_OK && len <= key->max_len) {
            out->v.blob.p = bbuf;
            out->v.blob.len = len;
            *blen = len;
            set = true;
        } else {
            out->v.blob.p = bbuf;
            out->v.blob.len = 0;
            *blen = 0;
        }
        break;
    }
    default:
        return ESP_ERR_INVALID_STATE;
    }
    if (is_set) {
        *is_set = set;
    }
    return ESP_OK;
}

/* Compare a candidate value against the current effective value. Lock held.
 * Returns ESP_ERR_NO_MEM if the comparison could not be made — callers must
 * NOT guess "changed" in that case (spurious notifications). */
static esp_err_t value_equals_effective(const espos_cfg_ns_t *nd, const espos_cfg_key_t *key,
                                        const espos_cfg_value_t *val, bool *eq)
{
    espos_cfg_value_t cur;
    char *sbuf = NULL;
    uint8_t *bbuf = NULL;
    size_t blen = 0;
    *eq = false;
    if (key->type == ESPOS_CFG_TYPE_STRING) {
        sbuf = malloc(key->max_len + 1);
        if (!sbuf) {
            return ESP_ERR_NO_MEM;
        }
    } else if (key->type == ESPOS_CFG_TYPE_BLOB) {
        bbuf = malloc(key->max_len ? key->max_len : 1);
        if (!bbuf) {
            return ESP_ERR_NO_MEM;
        }
    }
    esp_err_t err = espos_config_read_effective(nd, key, &cur, sbuf, bbuf, &blen, NULL);
    if (err == ESP_OK) {
        switch (key->type) {
        case ESPOS_CFG_TYPE_BOOL: *eq = cur.v.b == val->v.b; break;
        case ESPOS_CFG_TYPE_INT: *eq = cur.v.i == val->v.i; break;
        case ESPOS_CFG_TYPE_FLOAT: *eq = cur.v.f == val->v.f; break;
        case ESPOS_CFG_TYPE_STRING: *eq = strcmp(cur.v.s, val->v.s) == 0; break;
        case ESPOS_CFG_TYPE_BLOB:
            *eq = cur.v.blob.len == val->v.blob.len &&
                  (val->v.blob.len == 0 || memcmp(cur.v.blob.p, val->v.blob.p, val->v.blob.len) == 0);
            break;
        }
    }
    free(sbuf);
    free(bbuf);
    return err;
}

/* Write one validated value. Lock held. *changed reports effective change. */
static esp_err_t write_locked(ns_state_t *nss, const espos_cfg_key_t *key,
                              const espos_cfg_value_t *val, bool *changed)
{
    bool eq = false;
    esp_err_t err = value_equals_effective(nss->desc, key, val, &eq);
    if (err != ESP_OK) {
        *changed = false;
        return err;
    }
    *changed = !eq;
    switch (key->type) {
    case ESPOS_CFG_TYPE_BOOL: {
        uint8_t u = val->v.b ? 1 : 0;
        err = s.be->set(s.be_ctx, nss->h, key->name, key->type, &u, sizeof(u));
        break;
    }
    case ESPOS_CFG_TYPE_INT:
        err = s.be->set(s.be_ctx, nss->h, key->name, key->type, &val->v.i, sizeof(val->v.i));
        break;
    case ESPOS_CFG_TYPE_FLOAT:
        err = s.be->set(s.be_ctx, nss->h, key->name, key->type, &val->v.f, sizeof(val->v.f));
        break;
    case ESPOS_CFG_TYPE_STRING:
        err = s.be->set(s.be_ctx, nss->h, key->name, key->type, val->v.s, strlen(val->v.s) + 1);
        break;
    case ESPOS_CFG_TYPE_BLOB:
        if (val->v.blob.len == 0) {
            /* empty blob == absent */
            err = s.be->erase_key(s.be_ctx, nss->h, key->name);
            if (err == ESP_ERR_NOT_FOUND) {
                err = ESP_OK;
            }
        } else {
            err = s.be->set(s.be_ctx, nss->h, key->name, key->type, val->v.blob.p, val->v.blob.len);
        }
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    if (err != ESP_OK) {
        *changed = false;
        return err;
    }
    err = s.be->commit(s.be_ctx, nss->h);
    if (err != ESP_OK) {
        *changed = false;
    }
    return err;
}

/* Erase one key. Lock held. */
static esp_err_t reset_locked(ns_state_t *nss, const espos_cfg_key_t *key, bool *changed)
{
    /* Changed iff a stored value differs from the default; equivalently the
     * effective value != default. Compute by comparing against the default. */
    espos_cfg_value_t def = { .type = key->type };
    switch (key->type) {
    case ESPOS_CFG_TYPE_BOOL: def.v.b = key->def.b; break;
    case ESPOS_CFG_TYPE_INT: def.v.i = key->def.i; break;
    case ESPOS_CFG_TYPE_FLOAT: def.v.f = key->def.f; break;
    case ESPOS_CFG_TYPE_STRING: def.v.s = key->def.s; break;
    case ESPOS_CFG_TYPE_BLOB: def.v.blob.p = NULL; def.v.blob.len = 0; break;
    }
    bool eq = false;
    esp_err_t err = value_equals_effective(nss->desc, key, &def, &eq);
    if (err != ESP_OK) {
        *changed = false;
        return err;
    }
    *changed = !eq;
    err = s.be->erase_key(s.be_ctx, nss->h, key->name);
    if (err == ESP_ERR_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = s.be->commit(s.be_ctx, nss->h);
    }
    if (err != ESP_OK) {
        *changed = false;
    }
    return err;
}

/* -------------------------------------------------------------- notifying */

static void notify(const espos_cfg_ns_t *nd, const espos_cfg_key_t *key)
{
    /* Snapshot subscribers under the lock, call outside it. */
    subscriber_t subs[CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS];
    cfg_lock();
    memcpy(subs, s.subs, sizeof(subs));
    cfg_unlock();
    for (size_t i = 0; i < CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS; i++) {
        if (subs[i].cb) {
            subs[i].cb(nd->name, key->name, subs[i].arg);
        }
    }
}

esp_err_t espos_config_subscribe(espos_config_change_cb_t cb, void *arg)
{
    if (!cb || !s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    cfg_lock();
    for (size_t i = 0; i < CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS; i++) {
        if (!s.subs[i].cb) {
            s.subs[i].cb = cb;
            s.subs[i].arg = arg;
            err = ESP_OK;
            break;
        }
    }
    cfg_unlock();
    return err;
}

esp_err_t espos_config_unsubscribe(espos_config_change_cb_t cb, void *arg)
{
    if (!s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    cfg_lock();
    for (size_t i = 0; i < CONFIG_ESPOS_CONFIG_MAX_SUBSCRIBERS; i++) {
        if (s.subs[i].cb == cb && s.subs[i].arg == arg) {
            s.subs[i].cb = NULL;
            s.subs[i].arg = NULL;
            err = ESP_OK;
            break;
        }
    }
    cfg_unlock();
    return err;
}

/* ------------------------------------------------------------- migrations */

esp_err_t espos_config_register_migration(const char *ns, uint16_t from_version,
                                          espos_config_migrate_fn_t fn, void *arg)
{
    const espos_cfg_ns_t *nd = espos_config_find_ns(ns);
    /* Versions start at 1; an unstamped namespace is fresh, so a step "from 0"
     * would never run. */
    if (!nd || !fn || from_version == 0 || from_version >= nd->version) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    for (size_t i = 0; i < s.migration_count; i++) {
        if (s.migrations[i].from == from_version && strcmp(s.migrations[i].ns, ns) == 0) {
            return ESP_ERR_INVALID_STATE; /* duplicate step */
        }
    }
    if (s.migration_count >= CONFIG_ESPOS_CONFIG_MAX_MIGRATIONS) {
        return ESP_ERR_NO_MEM;
    }
    s.migrations[s.migration_count++] = (migration_t) { .ns = nd->name, .from = from_version, .fn = fn, .arg = arg };
    return ESP_OK;
}

static migration_t *find_migration(const char *ns, uint16_t from)
{
    for (size_t i = 0; i < s.migration_count; i++) {
        if (s.migrations[i].from == from && strcmp(s.migrations[i].ns, ns) == 0) {
            return &s.migrations[i];
        }
    }
    return NULL;
}

esp_err_t espos_config_migrate_get(espos_config_migrate_ctx_t *ctx, const char *key,
                                   espos_cfg_type_t type, void *buf, size_t *len)
{
    return s.be->get(s.be_ctx, ctx->ns->h, key, type, buf, len);
}

esp_err_t espos_config_migrate_set(espos_config_migrate_ctx_t *ctx, const char *key,
                                   espos_cfg_type_t type, const void *buf, size_t len)
{
    return s.be->set(s.be_ctx, ctx->ns->h, key, type, buf, len);
}

esp_err_t espos_config_migrate_erase(espos_config_migrate_ctx_t *ctx, const char *key)
{
    return s.be->erase_key(s.be_ctx, ctx->ns->h, key);
}

uint16_t espos_config_migrate_from_version(const espos_config_migrate_ctx_t *ctx)
{
    return ctx->from;
}

static esp_err_t read_version(ns_state_t *nss, uint16_t *out)
{
    int32_t v = 0;
    size_t len = sizeof(v);
    esp_err_t err = s.be->get(s.be_ctx, nss->h, ESPOS_CFG_VERSION_KEY, ESPOS_CFG_TYPE_INT, &v, &len);
    if (err != ESP_OK) {
        return err;
    }
    if (v < 0 || v > 65535) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = (uint16_t)v;
    return ESP_OK;
}

static esp_err_t write_version(ns_state_t *nss, uint16_t v)
{
    int32_t i = v;
    esp_err_t err = s.be->set(s.be_ctx, nss->h, ESPOS_CFG_VERSION_KEY, ESPOS_CFG_TYPE_INT, &i, sizeof(i));
    if (err == ESP_OK) {
        err = s.be->commit(s.be_ctx, nss->h);
    }
    return err;
}

/* Run the migration chain for one namespace. Lock held. */
static void migrate_ns(ns_state_t *nss)
{
    const espos_cfg_ns_t *nd = nss->desc;
    uint16_t stored = 0;
    esp_err_t err = read_version(nss, &stored);
    if (err == ESP_ERR_NOT_FOUND) {
        /* Fresh namespace: stamp and go. */
        err = write_version(nss, nd->version);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s: cannot stamp version %u (%s)", nd->name, nd->version, esp_err_to_name(err));
        }
        return;
    }
    if (err != ESP_OK) {
        /* Corrupt/wrong-type stamp: treat as fresh (values are validated on read anyway). */
        ESP_LOGW(TAG, "%s: bad version stamp (%s), re-stamping %u", nd->name, esp_err_to_name(err), nd->version);
        (void)s.be->erase_key(s.be_ctx, nss->h, ESPOS_CFG_VERSION_KEY);
        (void)write_version(nss, nd->version);
        return;
    }
    if (stored == nd->version) {
        return;
    }
    if (stored > nd->version) {
        ESP_LOGW(TAG, "%s: stored version %u newer than firmware %u; not migrating",
                 nd->name, stored, nd->version);
        return;
    }
    for (uint16_t v = stored; v < nd->version; v++) {
        migration_t *m = find_migration(nd->name, v);
        if (m) {
            struct espos_config_migrate_ctx ctx = { .ns = nss, .from = v };
            err = m->fn(&ctx, m->arg);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "%s: migration %u->%u failed (%s); staying at %u",
                         nd->name, v, v + 1, esp_err_to_name(err), v);
                return;
            }
            ESP_LOGI(TAG, "%s: migrated %u->%u", nd->name, v, v + 1);
        } else {
            ESP_LOGI(TAG, "%s: no migration for %u->%u, assuming additive change", nd->name, v, v + 1);
        }
        err = write_version(nss, v + 1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: cannot stamp version %u (%s)", nd->name, v + 1, esp_err_to_name(err));
            return;
        }
    }
}

esp_err_t espos_config_get_version(const char *ns, uint16_t *stored, uint16_t *current)
{
    const espos_cfg_ns_t *nd = espos_config_find_ns(ns);
    if (!nd) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    cfg_lock();
    uint16_t v = 0;
    if (read_version(ns_state_for(nd), &v) != ESP_OK) {
        v = 0;
    }
    cfg_unlock();
    if (stored) {
        *stored = v;
    }
    if (current) {
        *current = nd->version;
    }
    return ESP_OK;
}

/* -------------------------------------------------------------- lifecycle */

static esp_err_t open_all_locked(void)
{
    for (size_t i = 0; i < s.ns_count; i++) {
        esp_err_t err = s.be->open(s.be_ctx, s.ns[i].desc->name, &s.ns[i].h);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "cannot open namespace '%s': %s", s.ns[i].desc->name, esp_err_to_name(err));
            for (size_t j = 0; j < i; j++) {
                s.be->close(s.be_ctx, s.ns[j].h);
                s.ns[j].h = NULL;
            }
            return err;
        }
    }
    return ESP_OK;
}

static void close_all_locked(void)
{
    for (size_t i = 0; i < s.ns_count; i++) {
        if (s.ns[i].h) {
            s.be->close(s.be_ctx, s.ns[i].h);
            s.ns[i].h = NULL;
        }
    }
}

esp_err_t espos_config_init(const espos_config_backend_t *backend, void *backend_ctx)
{
    if (s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!backend) {
        backend = espos_config_backend_nvs();
    }
    if (!backend) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    s.be = backend;
    s.be_ctx = backend_ctx;
    if (!s.lock) {
        s.lock = xSemaphoreCreateMutex();
        if (!s.lock) {
            return ESP_ERR_NO_MEM;
        }
    }
    s.ns_count = espos_cfg_namespace_count;
    s.ns = calloc(s.ns_count ? s.ns_count : 1, sizeof(ns_state_t));
    if (!s.ns) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t i = 0; i < s.ns_count; i++) {
        s.ns[i].desc = &espos_cfg_namespaces[i];
    }
    bool reset = false;
    esp_err_t err = s.be->init(s.be_ctx, &reset);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "backend init failed: %s", esp_err_to_name(err));
        free(s.ns);
        s.ns = NULL;
        return err;
    }
    s.storage_reset = reset;
    if (reset) {
        ESP_LOGW(TAG, "storage was reset during init; all values are defaults");
    }
    cfg_lock();
    err = open_all_locked();
    if (err == ESP_OK) {
        /* Migrations run before the store is "inited": a migration callback
         * that calls the public API gets ESP_ERR_INVALID_STATE instead of
         * deadlocking on the (non-recursive) mutex it already holds. */
        for (size_t i = 0; i < s.ns_count; i++) {
            migrate_ns(&s.ns[i]);
        }
        s.inited = true;
    }
    cfg_unlock();
    if (err != ESP_OK) {
        free(s.ns);
        s.ns = NULL;
        return err;
    }
    ESP_LOGI(TAG, "ready: %u namespace(s), schema %s", (unsigned)s.ns_count, espos_cfg_schema_etag);
    return ESP_OK;
}

void espos_config_deinit(void)
{
    if (!s.inited) {
        return;
    }
    cfg_lock();
    close_all_locked();
    s.inited = false;
    free(s.ns);
    s.ns = NULL;
    s.ns_count = 0;
    s.migration_count = 0;
    memset(s.subs, 0, sizeof(s.subs));
    cfg_unlock();
}

bool espos_config_storage_was_reset(void)
{
    return s.storage_reset;
}

esp_err_t espos_config_factory_reset(void)
{
    if (!s.inited) {
        return ESP_ERR_INVALID_STATE;
    }
    cfg_lock();
    close_all_locked();
    esp_err_t err = s.be->erase_storage(s.be_ctx);
    if (err == ESP_OK) {
        bool dummy = false;
        err = s.be->init(s.be_ctx, &dummy);
    }
    if (err == ESP_OK) {
        err = open_all_locked();
    }
    if (err == ESP_OK) {
        for (size_t i = 0; i < s.ns_count; i++) {
            migrate_ns(&s.ns[i]); /* stamps fresh versions */
        }
    } else {
        /* Unusable until reboot; release what we hold so a later
         * espos_config_init() (or deinit) does not leak. */
        ESP_LOGE(TAG, "factory reset left the store unusable: %s", esp_err_to_name(err));
        s.inited = false;
        free(s.ns);
        s.ns = NULL;
        s.ns_count = 0;
    }
    cfg_unlock();
    return err;
}

/* -------------------------------------------------------------- accessors */

#define CHECK_INITED() do { if (!s.inited) { return ESP_ERR_INVALID_STATE; } } while (0)

static esp_err_t get_scalar(const char *ns, const char *key, espos_cfg_type_t type, espos_cfg_value_t *out)
{
    CHECK_INITED();
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    esp_err_t err = lookup(ns, key, &nss, &kd);
    if (err != ESP_OK) {
        return err;
    }
    if (kd->type != type) {
        return ESP_ERR_INVALID_ARG;
    }
    cfg_lock();
    size_t blen = 0;
    err = espos_config_read_effective(nss->desc, kd, out, NULL, NULL, &blen, NULL);
    cfg_unlock();
    return err;
}

esp_err_t espos_config_get_bool(const char *ns, const char *key, bool *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_cfg_value_t v;
    esp_err_t err = get_scalar(ns, key, ESPOS_CFG_TYPE_BOOL, &v);
    if (err == ESP_OK) {
        *out = v.v.b;
    }
    return err;
}

esp_err_t espos_config_get_i32(const char *ns, const char *key, int32_t *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_cfg_value_t v;
    esp_err_t err = get_scalar(ns, key, ESPOS_CFG_TYPE_INT, &v);
    if (err == ESP_OK) {
        *out = v.v.i;
    }
    return err;
}

esp_err_t espos_config_get_float(const char *ns, const char *key, float *out)
{
    if (!out) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_cfg_value_t v;
    esp_err_t err = get_scalar(ns, key, ESPOS_CFG_TYPE_FLOAT, &v);
    if (err == ESP_OK) {
        *out = v.v.f;
    }
    return err;
}

esp_err_t espos_config_get_str(const char *ns, const char *key, char *buf, size_t buf_size, size_t *out_len)
{
    CHECK_INITED();
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    esp_err_t err = lookup(ns, key, &nss, &kd);
    if (err != ESP_OK) {
        return err;
    }
    if (kd->type != ESPOS_CFG_TYPE_STRING) {
        return ESP_ERR_INVALID_ARG;
    }
    char *tmp = malloc(kd->max_len + 1);
    if (!tmp) {
        return ESP_ERR_NO_MEM;
    }
    espos_cfg_value_t v;
    size_t blen = 0;
    cfg_lock();
    err = espos_config_read_effective(nss->desc, kd, &v, tmp, NULL, &blen, NULL);
    cfg_unlock();
    if (err == ESP_OK) {
        size_t n = strlen(tmp);
        if (out_len) {
            *out_len = n;
        }
        if (buf && buf_size) {
            size_t c = n < buf_size - 1 ? n : buf_size - 1;
            memcpy(buf, tmp, c);
            buf[c] = '\0';
            if (n > c) {
                err = ESP_ERR_INVALID_SIZE;
            }
        } else if (buf_size == 0 && n > 0) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    free(tmp);
    return err;
}

esp_err_t espos_config_get_blob(const char *ns, const char *key, void *buf, size_t *len)
{
    CHECK_INITED();
    if (!len) {
        return ESP_ERR_INVALID_ARG;
    }
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    esp_err_t err = lookup(ns, key, &nss, &kd);
    if (err != ESP_OK) {
        return err;
    }
    if (kd->type != ESPOS_CFG_TYPE_BLOB) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t *tmp = malloc(kd->max_len ? kd->max_len : 1);
    if (!tmp) {
        return ESP_ERR_NO_MEM;
    }
    espos_cfg_value_t v;
    size_t blen = 0;
    cfg_lock();
    err = espos_config_read_effective(nss->desc, kd, &v, NULL, tmp, &blen, NULL);
    cfg_unlock();
    if (err == ESP_OK) {
        if (buf) {
            if (*len < blen) {
                err = ESP_ERR_INVALID_SIZE;
            } else {
                memcpy(buf, tmp, blen);
            }
        }
        *len = blen;
    }
    free(tmp);
    return err;
}

bool espos_config_is_set(const char *ns, const char *key)
{
    if (!s.inited) {
        return false;
    }
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    if (lookup(ns, key, &nss, &kd) != ESP_OK) {
        return false;
    }
    char *sbuf = NULL;
    uint8_t *bbuf = NULL;
    if (kd->type == ESPOS_CFG_TYPE_STRING) {
        sbuf = malloc(kd->max_len + 1);
        if (!sbuf) {
            return false;
        }
    } else if (kd->type == ESPOS_CFG_TYPE_BLOB) {
        bbuf = malloc(kd->max_len ? kd->max_len : 1);
        if (!bbuf) {
            return false;
        }
    }
    espos_cfg_value_t v;
    size_t blen = 0;
    bool set = false;
    cfg_lock();
    (void)espos_config_read_effective(nss->desc, kd, &v, sbuf, bbuf, &blen, &set);
    cfg_unlock();
    free(sbuf);
    free(bbuf);
    return set;
}

static esp_err_t set_value(const char *ns, const char *key, const espos_cfg_value_t *val)
{
    CHECK_INITED();
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    esp_err_t err = lookup(ns, key, &nss, &kd);
    if (err != ESP_OK) {
        return err;
    }
    char msg[64];
    if (!espos_config_validate(kd, val, msg, sizeof(msg))) {
        ESP_LOGW(TAG, "%s.%s rejected: %s", ns, key, msg);
        return ESP_ERR_INVALID_ARG;
    }
    bool changed = false;
    cfg_lock();
    err = write_locked(nss, kd, val, &changed);
    cfg_unlock();
    if (err == ESP_OK && changed) {
        notify(nss->desc, kd);
    }
    return err;
}

esp_err_t espos_config_set_bool(const char *ns, const char *key, bool v)
{
    espos_cfg_value_t val = { .type = ESPOS_CFG_TYPE_BOOL, .v.b = v };
    return set_value(ns, key, &val);
}

esp_err_t espos_config_set_i32(const char *ns, const char *key, int32_t v)
{
    espos_cfg_value_t val = { .type = ESPOS_CFG_TYPE_INT, .v.i = v };
    return set_value(ns, key, &val);
}

esp_err_t espos_config_set_float(const char *ns, const char *key, float v)
{
    espos_cfg_value_t val = { .type = ESPOS_CFG_TYPE_FLOAT, .v.f = v };
    return set_value(ns, key, &val);
}

esp_err_t espos_config_set_str(const char *ns, const char *key, const char *v)
{
    if (!v) {
        return ESP_ERR_INVALID_ARG;
    }
    espos_cfg_value_t val = { .type = ESPOS_CFG_TYPE_STRING, .v.s = v };
    return set_value(ns, key, &val);
}

esp_err_t espos_config_set_blob(const char *ns, const char *key, const void *buf, size_t len)
{
    espos_cfg_value_t val = { .type = ESPOS_CFG_TYPE_BLOB, .v.blob.p = buf, .v.blob.len = len };
    return set_value(ns, key, &val);
}

esp_err_t espos_config_reset_key(const char *ns, const char *key)
{
    CHECK_INITED();
    ns_state_t *nss;
    const espos_cfg_key_t *kd;
    esp_err_t err = lookup(ns, key, &nss, &kd);
    if (err != ESP_OK) {
        return err;
    }
    bool changed = false;
    cfg_lock();
    err = reset_locked(nss, kd, &changed);
    cfg_unlock();
    if (err == ESP_OK && changed) {
        notify(nss->desc, kd);
    }
    return err;
}

esp_err_t espos_config_reset_ns(const char *ns)
{
    CHECK_INITED();
    const espos_cfg_ns_t *nd = espos_config_find_ns(ns);
    if (!nd) {
        return ESP_ERR_NOT_FOUND;
    }
    ns_state_t *nss = ns_state_for(nd);
    bool *changed = calloc(nd->key_count ? nd->key_count : 1, sizeof(bool));
    if (!changed) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    cfg_lock();
    for (size_t i = 0; i < nd->key_count; i++) {
        bool c = false;
        esp_err_t e = reset_locked(nss, &nd->keys[i], &c);
        changed[i] = c;
        if (e != ESP_OK && err == ESP_OK) {
            err = e;
        }
    }
    cfg_unlock();
    for (size_t i = 0; i < nd->key_count; i++) {
        if (changed[i]) {
            notify(nd, &nd->keys[i]);
        }
    }
    free(changed);
    return err;
}

/* ------------------------------------------------------------- batch plan */

esp_err_t espos_config_apply_plan(const espos_config_plan_entry_t *plan, size_t n,
                                  size_t *changed_idx, size_t *changed_count,
                                  bool *restart_required)
{
    CHECK_INITED();
    esp_err_t err = ESP_OK;
    size_t nchanged = 0;
    bool restart = false;
    cfg_lock();
    for (size_t i = 0; i < n; i++) {
        ns_state_t *nss = ns_state_for(plan[i].ns);
        bool c = false;
        esp_err_t e = plan[i].reset ? reset_locked(nss, plan[i].key, &c)
                                    : write_locked(nss, plan[i].key, &plan[i].val, &c);
        if (e != ESP_OK) {
            ESP_LOGE(TAG, "%s.%s: write failed (%s)", plan[i].ns->name, plan[i].key->name, esp_err_to_name(e));
            if (err == ESP_OK) {
                err = e;
            }
            continue;
        }
        if (c) {
            changed_idx[nchanged++] = i;
            if (plan[i].key->flags & ESPOS_CFG_FLAG_RESTART_REQUIRED) {
                restart = true;
            }
        }
    }
    cfg_unlock();
    for (size_t i = 0; i < nchanged; i++) {
        notify(plan[changed_idx[i]].ns, plan[changed_idx[i]].key);
    }
    *changed_count = nchanged;
    *restart_required = restart;
    return err;
}
