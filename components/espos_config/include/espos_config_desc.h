/*
 * SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
 *
 * espOS config descriptor tables.
 *
 * These types are instantiated by the build-time generator
 * (tools/espos_gen_config.py) from every registered config descriptor. The
 * runtime never spells defaults, ranges or key names by hand — it consults
 * these tables.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESPOS_CFG_TYPE_BOOL = 1,
    ESPOS_CFG_TYPE_INT,    /* int32_t */
    ESPOS_CFG_TYPE_FLOAT,  /* float, stored as its IEEE-754 bit pattern */
    ESPOS_CFG_TYPE_STRING, /* NUL-terminated UTF-8, ≤ max_len bytes excluding NUL */
    ESPOS_CFG_TYPE_BLOB,   /* opaque bytes, ≤ max_len; JSON form is base64 */
} espos_cfg_type_t;

#define ESPOS_CFG_FLAG_SECRET           (1u << 0) /* redacted on export, sentinel ignored on import */
#define ESPOS_CFG_FLAG_RESTART_REQUIRED (1u << 1) /* change takes effect after reboot */
/* Displayed but not editable: the UI greys the control out and a write is
 * rejected. A device-derived value (a serial number, a computed limit) that a
 * user must be able to read but never set. */
#define ESPOS_CFG_FLAG_READ_ONLY (1u << 2)

/* Presentation hints. They never change what is stored — only how a value is
 * shown and typed in. SI units are the on-device truth (SignalK's rule), but
 * nobody trims an anchor rode in radians, so the UI multiplies on read and
 * divides on write: display = stored * display_mul + display_off. A descriptor
 * that sets neither leaves both at their identity (1.0 / 0.0). */
typedef struct {
    const char *group;    /* UI tab, "" = the namespace's own tab */
    float display_mul;    /* 0 == unset, treated as 1 */
    float display_off;
    /* A string key whose contents are a JSON array of rows, edited as a
     * table. Columns are the JSON member names, in order; NULL = not a table.
     * The value stays a readable NVS string so an export is diffable. */
    const char *const *table_columns;
    size_t table_column_count;
} espos_cfg_display_t;

typedef union {
    bool b;
    int32_t i;
    float f;
    const char *s;
} espos_cfg_scalar_t;

typedef struct {
    const char *name;        /* NVS key, ≤ 15 chars */
    const char *title;
    const char *description;
    const char *unit;        /* "" if none */
    espos_cfg_type_t type;
    uint32_t flags;
    espos_cfg_scalar_t def;  /* compiled-in default (unused for blob) */
    espos_cfg_scalar_t min;  /* int: always valid; float: iff has_min */
    espos_cfg_scalar_t max;  /* int: always valid; float: iff has_max */
    bool has_min;
    bool has_max;
    size_t max_len;          /* string: bytes excl. NUL; blob: bytes */
    const char *const *enum_values; /* string only, may be NULL */
    size_t enum_count;
    /* Presentation only. A key that sets nothing here is all-zero, which the
     * runtime reads as "no group, identity scaling, not a table" — so every
     * descriptor written before these fields existed stays correct. */
    espos_cfg_display_t display;
} espos_cfg_key_t;

typedef struct {
    const char *name;    /* NVS namespace, ≤ 15 chars */
    const char *title;
    uint16_t version;    /* current descriptor version; stored under key "config_version" */
    const espos_cfg_key_t *keys;
    size_t key_count;
    /* Runtime namespaces only (see espos_config_register_ns): the human name
     * of what registered this, shown as the tab label's subtitle. NULL for a
     * build-time descriptor. */
    const char *description;
} espos_cfg_ns_t;

/* NVS caps both a namespace and a key name at 15 characters plus NUL. A node
 * that registers its own namespace gets the prefix below, so its id may be at
 * most 12 characters. */
#define ESPOS_CFG_RUNTIME_NS_PREFIX "f_"
#define ESPOS_CFG_RUNTIME_ID_MAX    12
#define ESPOS_CFG_NS_NAME_MAX       15

/* Emitted by the generator (sorted by namespace name). */
extern const espos_cfg_ns_t espos_cfg_namespaces[];
extern const size_t espos_cfg_namespace_count;

/* JSON Schema of the whole configuration document, generated at build time. */
extern const char espos_cfg_schema_json[];
extern const size_t espos_cfg_schema_json_len;
/* Short content hash of the schema text (usable as HTTP ETag). */
extern const char espos_cfg_schema_etag[];

/* Reserved key holding the per-namespace schema version. */
#define ESPOS_CFG_VERSION_KEY "config_version"

#ifdef __cplusplus
}
#endif
