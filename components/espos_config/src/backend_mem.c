/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * In-memory config backend: used by host tests and available on device for
 * diagnostics. Not persistent. Linear lists — sizes are tiny.
 */
#include <stdlib.h>
#include <string.h>

#include "espos_config_backend.h"

typedef struct entry {
    struct entry *next;
    char key[16];
    espos_cfg_type_t type;
    size_t len;
    uint8_t *data;
} entry_t;

typedef struct mem_ns {
    struct mem_ns *next;
    char name[16];
    entry_t *entries;
} mem_ns_t;

struct espos_config_mem {
    mem_ns_t *namespaces;
    esp_err_t write_fault;
    bool inited;
};

static void free_entries(mem_ns_t *ns)
{
    entry_t *e = ns->entries;
    while (e) {
        entry_t *n = e->next;
        free(e->data);
        free(e);
        e = n;
    }
    ns->entries = NULL;
}

static void free_all(espos_config_mem_t *m)
{
    mem_ns_t *ns = m->namespaces;
    while (ns) {
        mem_ns_t *n = ns->next;
        free_entries(ns);
        free(ns);
        ns = n;
    }
    m->namespaces = NULL;
}

espos_config_mem_t *espos_config_mem_create(void)
{
    return calloc(1, sizeof(espos_config_mem_t));
}

void espos_config_mem_destroy(espos_config_mem_t *m)
{
    if (!m) {
        return;
    }
    free_all(m);
    free(m);
}

void espos_config_mem_set_write_fault(espos_config_mem_t *m, esp_err_t err)
{
    m->write_fault = err;
}

static mem_ns_t *find_ns(espos_config_mem_t *m, const char *name, bool create)
{
    for (mem_ns_t *ns = m->namespaces; ns; ns = ns->next) {
        if (strcmp(ns->name, name) == 0) {
            return ns;
        }
    }
    if (!create || strlen(name) > 15) {
        return NULL;
    }
    mem_ns_t *ns = calloc(1, sizeof(*ns));
    if (!ns) {
        return NULL;
    }
    strcpy(ns->name, name);
    ns->next = m->namespaces;
    m->namespaces = ns;
    return ns;
}

static entry_t *find_entry(mem_ns_t *ns, const char *key)
{
    for (entry_t *e = ns->entries; e; e = e->next) {
        if (strcmp(e->key, key) == 0) {
            return e;
        }
    }
    return NULL;
}

static size_t scalar_size(espos_cfg_type_t t)
{
    switch (t) {
    case ESPOS_CFG_TYPE_BOOL: return 1;
    case ESPOS_CFG_TYPE_INT: return 4;
    case ESPOS_CFG_TYPE_FLOAT: return 4;
    default: return 0;
    }
}

static esp_err_t raw_set(mem_ns_t *ns, const char *key, espos_cfg_type_t type, const void *buf, size_t len)
{
    if (strlen(key) > 15) {
        return ESP_ERR_INVALID_ARG;
    }
    if (type == ESPOS_CFG_TYPE_STRING) {
        len = strlen((const char *)buf) + 1;
        if (len > 4000) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else if (type == ESPOS_CFG_TYPE_BLOB) {
        if (len > 508000) {
            return ESP_ERR_INVALID_SIZE;
        }
    } else {
        len = scalar_size(type);
        if (!len) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    entry_t *e = find_entry(ns, key);
    if (!e) {
        e = calloc(1, sizeof(*e));
        if (!e) {
            return ESP_ERR_NO_MEM;
        }
        strcpy(e->key, key);
        e->next = ns->entries;
        ns->entries = e;
    }
    uint8_t *d = malloc(len ? len : 1);
    if (!d) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(d, buf, len);
    free(e->data);
    e->data = d;
    e->len = len;
    e->type = type; /* NVS semantics: a set with a different type replaces (nvs_set_* on a
                       key of another type actually fails; we model the erase+set path) */
    return ESP_OK;
}

esp_err_t espos_config_mem_plant(espos_config_mem_t *m, const char *ns, const char *key,
                                 espos_cfg_type_t type, const void *buf, size_t len)
{
    mem_ns_t *n = find_ns(m, ns, true);
    if (!n) {
        return ESP_ERR_NO_MEM;
    }
    return raw_set(n, key, type, buf, len);
}

size_t espos_config_mem_key_count(espos_config_mem_t *m, const char *ns)
{
    mem_ns_t *n = find_ns(m, ns, false);
    size_t c = 0;
    for (entry_t *e = n ? n->entries : NULL; e; e = e->next) {
        c++;
    }
    return c;
}

bool espos_config_mem_has_key(espos_config_mem_t *m, const char *ns, const char *key)
{
    mem_ns_t *n = find_ns(m, ns, false);
    return n && find_entry(n, key) != NULL;
}

/* ---------------------------------------------------------- backend vtable */

static esp_err_t be_init(void *ctx, bool *storage_reset)
{
    espos_config_mem_t *m = ctx;
    *storage_reset = false;
    m->inited = true;
    return ESP_OK;
}

static esp_err_t be_open(void *ctx, const char *ns, espos_config_bh_t *out)
{
    espos_config_mem_t *m = ctx;
    if (!m->inited) {
        return ESP_ERR_INVALID_STATE;
    }
    mem_ns_t *n = find_ns(m, ns, true);
    if (!n) {
        return ESP_ERR_NO_MEM;
    }
    *out = n;
    return ESP_OK;
}

static void be_close(void *ctx, espos_config_bh_t h)
{
    (void)ctx;
    (void)h;
}

static esp_err_t be_get(void *ctx, espos_config_bh_t h, const char *key, espos_cfg_type_t type,
                        void *buf, size_t *len)
{
    (void)ctx;
    mem_ns_t *ns = h;
    entry_t *e = find_entry(ns, key);
    if (!e) {
        return ESP_ERR_NOT_FOUND;
    }
    if (e->type != type) {
        return ESP_ERR_INVALID_STATE;
    }
    if (type == ESPOS_CFG_TYPE_STRING || type == ESPOS_CFG_TYPE_BLOB) {
        if (!buf) {
            *len = e->len;
            return ESP_OK;
        }
        if (*len < e->len) {
            *len = e->len;
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(buf, e->data, e->len);
        *len = e->len;
        return ESP_OK;
    }
    if (*len < e->len) {
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}

static esp_err_t be_set(void *ctx, espos_config_bh_t h, const char *key, espos_cfg_type_t type,
                        const void *buf, size_t len)
{
    espos_config_mem_t *m = ctx;
    if (m->write_fault != ESP_OK) {
        return m->write_fault;
    }
    return raw_set(h, key, type, buf, len);
}

static esp_err_t be_erase_key(void *ctx, espos_config_bh_t h, const char *key)
{
    espos_config_mem_t *m = ctx;
    if (m->write_fault != ESP_OK) {
        return m->write_fault;
    }
    mem_ns_t *ns = h;
    entry_t **pp = &ns->entries;
    while (*pp) {
        if (strcmp((*pp)->key, key) == 0) {
            entry_t *e = *pp;
            *pp = e->next;
            free(e->data);
            free(e);
            return ESP_OK;
        }
        pp = &(*pp)->next;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t be_erase_all(void *ctx, espos_config_bh_t h)
{
    espos_config_mem_t *m = ctx;
    if (m->write_fault != ESP_OK) {
        return m->write_fault;
    }
    free_entries(h);
    return ESP_OK;
}

static esp_err_t be_commit(void *ctx, espos_config_bh_t h)
{
    espos_config_mem_t *m = ctx;
    (void)h;
    return m->write_fault;
}

static esp_err_t be_erase_storage(void *ctx)
{
    espos_config_mem_t *m = ctx;
    free_all(m);
    m->inited = false;
    return ESP_OK;
}

static const espos_config_backend_t k_mem_backend = {
    .init = be_init,
    .open = be_open,
    .close = be_close,
    .get = be_get,
    .set = be_set,
    .erase_key = be_erase_key,
    .erase_all = be_erase_all,
    .commit = be_commit,
    .erase_storage = be_erase_storage,
};

const espos_config_backend_t *espos_config_backend_mem(void)
{
    return &k_mem_backend;
}
