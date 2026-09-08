/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Namespaces a node registers at run time: the same store, the same
 * validation, the same export/import and the same schema as a compiled
 * descriptor — only without a build-time file. What these tests guard is that
 * the two paths stay indistinguishable to everything downstream.
 */
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "test_common.h"
#include "espos_config_desc.h"

/* A node's ParamSet: static storage, because espos_config_register_ns()
 * borrows the descriptor and never copies it. This is the shape a real
 * Linear("cal", …) hands over. */
static const espos_cfg_key_t s_cal_keys[] = {
    { .name = "mul", .title = "Multiplier", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_FLOAT, .def.f = 1.0f, .has_min = true, .min.f = -100.0f, .has_max = true, .max.f = 100.0f },
    { .name = "off", .title = "Offset", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_FLOAT, .def.f = 0.0f },
    { .name = "label", .title = "Label", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_STRING, .def.s = "cal", .max_len = 16 },
};
static const espos_cfg_ns_t s_cal_ns = {
    .name = "f_cal",
    .title = "Calibration",
    .version = 1,
    .keys = s_cal_keys,
    .key_count = 3,
    .description = "Linear node \"cal\"",
};

/* A second node, to prove the table holds more than one. */
static const espos_cfg_key_t s_rate_keys[] = {
    { .name = "hz", .title = "Rate", .description = "", .unit = "Hz", .type = ESPOS_CFG_TYPE_INT, .def.i = 10, .min.i = 1, .max.i = 100 },
};
static const espos_cfg_ns_t s_rate_ns = {
    .name = "f_rate",
    .title = "Rate",
    .version = 1,
    .keys = s_rate_keys,
    .key_count = 1,
};

/* A curve table: string key holding JSON rows, the shape task F2 asks for. */
static const char *const s_curve_cols[] = { "input", "output" };
static const espos_cfg_key_t s_curve_keys[] = {
    { .name = "points", .title = "Curve", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_STRING, .def.s = "[]", .max_len = 3999, .display = { .table_columns = s_curve_cols, .table_column_count = 2 } },
};
static const espos_cfg_ns_t s_curve_ns = {
    .name = "f_curve",
    .title = "Curve",
    .version = 1,
    .keys = s_curve_keys,
    .key_count = 1,
};

/* ------------------------------------------------------------------ naming */

TEST_CASE("flow namespace names are built and bounded by the NVS limit", "[runtime]")
{
    char n[ESPOS_CFG_NS_NAME_MAX + 1];
    TEST_ESP_OK(espos_config_flow_ns_name("cal", n, sizeof(n)));
    TEST_ASSERT_EQUAL_STRING("f_cal", n);
    /* exactly at the limit: 12 characters of id + the 2-character prefix */
    TEST_ESP_OK(espos_config_flow_ns_name("abcdefghijkl", n, sizeof(n)));
    TEST_ASSERT_EQUAL_STRING("f_abcdefghijkl", n);
    TEST_ASSERT_EQUAL(ESPOS_CFG_NS_NAME_MAX - 1, strlen(n));
    /* one over: rejected, never truncated into a name that collides */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name("abcdefghijklm", n, sizeof(n)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name("", n, sizeof(n)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name("Cal", n, sizeof(n)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name("has-dash", n, sizeof(n)));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name(NULL, n, sizeof(n)));
    /* a buffer that cannot hold the maximum name is refused up front */
    char tiny[4];
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_flow_ns_name("cal", tiny, sizeof(tiny)));
}

/* ------------------------------------------------------- register/unregister */

TEST_CASE("register makes a namespace behave like a compiled one", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ASSERT_EQUAL(0, espos_config_runtime_ns_count());
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    TEST_ASSERT_EQUAL(1, espos_config_runtime_ns_count());

    /* found across both tables */
    const espos_cfg_ns_t *nd = espos_config_find_ns("f_cal");
    TEST_ASSERT_EQUAL_PTR(&s_cal_ns, nd);
    TEST_ASSERT_NOT_NULL(espos_config_find_key(nd, "mul"));
    TEST_ASSERT_NULL(espos_config_find_key(nd, "nope"));
    /* the static table still resolves */
    TEST_ASSERT_NOT_NULL(espos_config_find_ns(ESPOS_CFG_NS_T1));

    /* defaults, then a validated write, then read back */
    float f = 0;
    TEST_ESP_OK(espos_config_get_float("f_cal", "mul", &f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, f);
    TEST_ASSERT_FALSE(espos_config_is_set("f_cal", "mul"));
    TEST_ESP_OK(espos_config_set_float("f_cal", "mul", 2.5f));
    TEST_ESP_OK(espos_config_get_float("f_cal", "mul", &f));
    TEST_ASSERT_EQUAL_FLOAT(2.5f, f);
    TEST_ASSERT_TRUE(espos_config_is_set("f_cal", "mul"));
    /* range validation applies exactly as it does for a static key */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_float("f_cal", "mul", 1000.0f));
    /* the version stamp was written on registration */
    uint16_t stored = 0, current = 0;
    TEST_ESP_OK(espos_config_get_version("f_cal", &stored, &current));
    TEST_ASSERT_EQUAL(1, stored);
    TEST_ASSERT_EQUAL(1, current);

    TEST_ESP_OK(espos_config_unregister_ns("f_cal"));
    TEST_ASSERT_EQUAL(0, espos_config_runtime_ns_count());
    TEST_ASSERT_NULL(espos_config_find_ns("f_cal"));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_get_float("f_cal", "mul", &f));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_unregister_ns("f_cal"));
    fixture_teardown(m);
}

TEST_CASE("values survive unregister and come back on re-register", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    TEST_ESP_OK(espos_config_set_float("f_cal", "off", -3.25f));
    /* Unregistering is "the node went away", not "forget its settings": a
     * graph rebuilt after a reboot must find its calibration where it was. */
    TEST_ESP_OK(espos_config_unregister_ns("f_cal"));
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    float f = 0;
    TEST_ESP_OK(espos_config_get_float("f_cal", "off", &f));
    TEST_ASSERT_EQUAL_FLOAT(-3.25f, f);
    /* reset_ns is how a caller actually discards them */
    TEST_ESP_OK(espos_config_reset_ns("f_cal"));
    TEST_ESP_OK(espos_config_get_float("f_cal", "off", &f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, f);
    fixture_teardown(m);
}

TEST_CASE("a duplicate or malformed descriptor fails loudly", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    /* same name twice: the second node would silently shadow the first */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_register_ns(&s_cal_ns));
    static const espos_cfg_ns_t same_name = {
        .name = "f_cal",
        .title = "Other",
        .version = 1,
        .keys = s_rate_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_register_ns(&same_name));
    /* a built-in namespace may not be shadowed either */
    static const espos_cfg_ns_t shadows_static = {
        .name = "t1",
        .title = "Hijack",
        .version = 1,
        .keys = s_rate_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_register_ns(&shadows_static));
    /* ... and cannot be unregistered */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_unregister_ns(ESPOS_CFG_NS_T1));

    /* over-long name: 16 characters, one past what NVS carries */
    static const espos_cfg_ns_t too_long = {
        .name = "f_abcdefghijklmn",
        .title = "Too long",
        .version = 1,
        .keys = s_rate_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&too_long));
    static const espos_cfg_ns_t bad_chars = {
        .name = "f_Cal",
        .title = "Caps",
        .version = 1,
        .keys = s_rate_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&bad_chars));
    /* structurally broken descriptors */
    static const espos_cfg_ns_t no_keys = { .name = "f_empty", .title = "E", .version = 1 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&no_keys));
    static const espos_cfg_ns_t no_version = {
        .name = "f_nover",
        .title = "N",
        .version = 0,
        .keys = s_rate_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&no_version));
    static const espos_cfg_key_t dup_keys[] = {
        { .name = "hz", .title = "a", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_INT },
        { .name = "hz", .title = "b", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_INT },
    };
    static const espos_cfg_ns_t dup_ns = {
        .name = "f_dup",
        .title = "D",
        .version = 1,
        .keys = dup_keys,
        .key_count = 2,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&dup_ns));
    static const espos_cfg_key_t reserved_keys[] = {
        { .name = "config_version", .title = "v", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_INT },
    };
    static const espos_cfg_ns_t reserved_ns = {
        .name = "f_resv",
        .title = "R",
        .version = 1,
        .keys = reserved_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&reserved_ns));
    /* a string key with no max_len would read past its buffer */
    static const espos_cfg_key_t nolen_keys[] = {
        { .name = "s", .title = "s", .description = "", .unit = "", .type = ESPOS_CFG_TYPE_STRING, .def.s = "" },
    };
    static const espos_cfg_ns_t nolen_ns = {
        .name = "f_nolen",
        .title = "N",
        .version = 1,
        .keys = nolen_keys,
        .key_count = 1,
    };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(&nolen_ns));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_ns(NULL));

    TEST_ASSERT_EQUAL(1, espos_config_runtime_ns_count());
    fixture_teardown(m);
}

TEST_CASE("registration before init is opened by init", "[runtime]")
{
    /* The graph is usually built after the store is up, but a node created
     * during static construction must not lose its settings. */
    TEST_ESP_OK(espos_config_register_ns(&s_rate_ns));
    TEST_ASSERT_EQUAL(1, espos_config_runtime_ns_count());
    espos_config_mem_t *m = fixture_setup();
    int32_t hz = 0;
    TEST_ESP_OK(espos_config_get_i32("f_rate", "hz", &hz));
    TEST_ASSERT_EQUAL_INT32(10, hz);
    TEST_ESP_OK(espos_config_set_i32("f_rate", "hz", 25));
    TEST_ESP_OK(espos_config_get_i32("f_rate", "hz", &hz));
    TEST_ASSERT_EQUAL_INT32(25, hz);
    fixture_teardown(m);
    /* deinit drops the registration: the descriptor belongs to a graph that
     * is gone, and a stale slot would open a namespace for freed memory. */
    TEST_ASSERT_EQUAL(0, espos_config_runtime_ns_count());
}

TEST_CASE("the runtime table has a hard limit and reports it", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    /* Names f_r000 … : one per slot, plus one that must not fit. */
    static char names[CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS + 1][8];
    static espos_cfg_ns_t many[CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS + 1];
    for (int i = 0; i <= CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS; i++) {
        snprintf(names[i], sizeof(names[i]), "f_r%03d", i);
        many[i] = (espos_cfg_ns_t) { .name = names[i], .title = "R", .version = 1, .keys = s_rate_keys, .key_count = 1 };
    }
    for (int i = 0; i < CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS; i++) {
        TEST_ESP_OK(espos_config_register_ns(&many[i]));
    }
    TEST_ASSERT_EQUAL(CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS, espos_config_runtime_ns_count());
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_config_register_ns(&many[CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS]));
    /* a freed slot is reused */
    TEST_ESP_OK(espos_config_unregister_ns(names[0]));
    TEST_ESP_OK(espos_config_register_ns(&many[CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS]));
    fixture_teardown(m);
}

/* ------------------------------------------------------------ export/import */

TEST_CASE("export covers runtime namespaces alongside static ones", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    TEST_ESP_OK(espos_config_set_float("f_cal", "mul", 3.5f));
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json(NULL, false, &txt));
    cJSON *doc = cJSON_Parse(txt);
    TEST_ASSERT_NOT_NULL(doc);
    cJSON *cal = cJSON_GetObjectItem(doc, "f_cal");
    TEST_ASSERT_NOT_NULL(cal);
    TEST_ASSERT_EQUAL_DOUBLE(3.5, cJSON_GetNumberValue(cJSON_GetObjectItem(cal, "mul")));
    TEST_ASSERT_EQUAL_STRING("cal", cJSON_GetStringValue(cJSON_GetObjectItem(cal, "label")));
    /* the static namespaces are still all there */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(doc, "t1"));
    cJSON_Delete(doc);
    free(txt);

    /* ?ns= restricted to the runtime namespace */
    TEST_ESP_OK(espos_config_export_json("f_cal", false, &txt));
    doc = cJSON_Parse(txt);
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(doc));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(doc, "f_cal"));
    cJSON_Delete(doc);
    free(txt);
    fixture_teardown(m);
}

TEST_CASE("export/import round trip through a runtime namespace", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    TEST_ESP_OK(espos_config_set_float("f_cal", "mul", 7.5f));
    TEST_ESP_OK(espos_config_set_str("f_cal", "label", "port tank"));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json(NULL, true, &txt));

    /* wipe both, then restore from the document */
    TEST_ESP_OK(espos_config_reset_ns("f_cal"));
    TEST_ESP_OK(espos_config_reset_ns(ESPOS_CFG_NS_T1));
    espos_config_import_result_t r;
    TEST_ESP_OK(espos_config_import_json(txt, strlen(txt), false, &r, NULL));
    free(txt);

    float f = 0;
    char s[24];
    int32_t i = 0;
    TEST_ESP_OK(espos_config_get_float("f_cal", "mul", &f));
    TEST_ASSERT_EQUAL_FLOAT(7.5f, f);
    TEST_ESP_OK(espos_config_get_str("f_cal", "label", s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("port tank", s);
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(7, i);
    fixture_teardown(m);
}

#define SET_MUL_4    "{\"f_cal\":{\"mul\":4}}"
#define SET_MUL_999  "{\"f_cal\":{\"mul\":999}}"
#define SET_UNKNOWN  "{\"f_cal\":{\"zz\":1}}"
#define SET_MUL_NULL "{\"f_cal\":{\"mul\":null}}"

TEST_CASE("import validates and notifies a runtime namespace", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));

    espos_config_import_result_t r;
    char *report = NULL;
    TEST_ESP_OK(espos_config_import_json(SET_MUL_4, strlen(SET_MUL_4), false, &r, &report));
    TEST_ASSERT_EQUAL(1, r.changed);
    TEST_ASSERT_TRUE(rec_has(&rec, "f_cal.mul"));
    free(report);

    /* out of range, and an unknown key inside a runtime namespace */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_config_import_json(SET_MUL_999, strlen(SET_MUL_999), false, &r, NULL));
    TEST_ASSERT_EQUAL_STRING("f_cal.mul", r.error_path);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_config_import_json(SET_UNKNOWN, strlen(SET_UNKNOWN), false, &r, NULL));
    TEST_ASSERT_EQUAL_STRING("f_cal.zz", r.error_path);
    /* null resets to the default */
    TEST_ESP_OK(espos_config_import_json(SET_MUL_NULL, strlen(SET_MUL_NULL), false, &r, NULL));
    float f = 0;
    TEST_ESP_OK(espos_config_get_float("f_cal", "mul", &f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, f);

    TEST_ESP_OK(espos_config_unsubscribe(rec_cb, &rec));
    /* once unregistered the namespace is unknown to import too */
    TEST_ESP_OK(espos_config_unregister_ns("f_cal"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG,
                      espos_config_import_json(SET_MUL_4, strlen(SET_MUL_4), false, &r, NULL));
    fixture_teardown(m);
}

TEST_CASE("a table key round-trips as a readable JSON string", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_curve_ns));
    /* The point of a string key: what NVS holds is what an export shows, and
     * the UI edits rows without a base64 detour. */
    const char *rows = "[[0,0],[0.5,1.2],[1,2.4]]";
    TEST_ESP_OK(espos_config_set_str("f_curve", "points", rows));
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json("f_curve", false, &txt));
    TEST_ASSERT_NOT_NULL(strstr(txt, "0.5"));  /* readable, not base64 */
    cJSON *doc = cJSON_Parse(txt);
    const char *got = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "f_curve"), "points"));
    TEST_ASSERT_EQUAL_STRING(rows, got);
    cJSON *parsed = cJSON_Parse(got);   /* and it really is a JSON array */
    TEST_ASSERT_TRUE(cJSON_IsArray(parsed));
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(parsed));
    cJSON_Delete(parsed);
    cJSON_Delete(doc);

    /* import it back after a reset */
    TEST_ESP_OK(espos_config_reset_ns("f_curve"));
    espos_config_import_result_t r;
    TEST_ESP_OK(espos_config_import_json(txt, strlen(txt), false, &r, NULL));
    free(txt);
    char back[64];
    TEST_ESP_OK(espos_config_get_str("f_curve", "points", back, sizeof(back), NULL));
    TEST_ASSERT_EQUAL_STRING(rows, back);

    /* over the declared length is rejected rather than truncated */
    char *big = malloc(4100);
    memset(big, 'x', 4099);
    big[4099] = '\0';
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str("f_curve", "points", big));
    free(big);
    fixture_teardown(m);
}

/* ------------------------------------------------------------------ schema */

static cJSON *schema_doc(char etag[ESPOS_CFG_ETAG_MAX])
{
    char *txt = NULL;
    TEST_ESP_OK(espos_config_schema_json(&txt, etag));
    TEST_ASSERT_NOT_NULL(txt);
    cJSON *doc = cJSON_Parse(txt);
    free(txt);
    TEST_ASSERT_NOT_NULL(doc);
    return doc;
}

TEST_CASE("the schema merges runtime namespaces and its ETag moves with them", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    char e0[ESPOS_CFG_ETAG_MAX], e1[ESPOS_CFG_ETAG_MAX], e2[ESPOS_CFG_ETAG_MAX];

    /* With nothing registered the document is the compiled one, byte for
     * byte, so a cached browser keeps its 304. */
    char *plain = NULL;
    TEST_ESP_OK(espos_config_schema_json(&plain, e0));
    TEST_ASSERT_EQUAL_STRING(espos_cfg_schema_json, plain);
    TEST_ASSERT_EQUAL_STRING(espos_cfg_schema_etag, e0);
    free(plain);

    TEST_ESP_OK(espos_config_register_ns(&s_cal_ns));
    cJSON *doc = schema_doc(e1);
    TEST_ASSERT_TRUE(strcmp(e0, e1) != 0);        /* register changes the ETag */
    TEST_ASSERT_TRUE(strlen(e1) < ESPOS_CFG_ETAG_MAX);
    cJSON *props = cJSON_GetObjectItem(doc, "properties");
    /* the static namespaces are untouched */
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(props, "t1"));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(props, "sec"));
    cJSON *cal = cJSON_GetObjectItem(props, "f_cal");
    TEST_ASSERT_NOT_NULL(cal);
    TEST_ASSERT_EQUAL_STRING("Calibration", cJSON_GetStringValue(cJSON_GetObjectItem(cal, "title")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(cal, "x-espos-runtime")));
    TEST_ASSERT_EQUAL(1, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(cal, "x-espos-version")));
    cJSON *ck = cJSON_GetObjectItem(cal, "properties");
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(ck));
    cJSON *mul = cJSON_GetObjectItem(ck, "mul");
    TEST_ASSERT_EQUAL_STRING("number", cJSON_GetStringValue(cJSON_GetObjectItem(mul, "type")));
    TEST_ASSERT_EQUAL_DOUBLE(1.0, cJSON_GetNumberValue(cJSON_GetObjectItem(mul, "default")));
    TEST_ASSERT_EQUAL_DOUBLE(-100.0, cJSON_GetNumberValue(cJSON_GetObjectItem(mul, "minimum")));
    TEST_ASSERT_EQUAL_DOUBLE(100.0, cJSON_GetNumberValue(cJSON_GetObjectItem(mul, "maximum")));
    cJSON *label = cJSON_GetObjectItem(ck, "label");
    TEST_ASSERT_EQUAL_STRING("string", cJSON_GetStringValue(cJSON_GetObjectItem(label, "type")));
    TEST_ASSERT_EQUAL_DOUBLE(16, cJSON_GetNumberValue(cJSON_GetObjectItem(label, "maxLength")));
    cJSON_Delete(doc);

    /* a second registration moves it again, and unregistering does too */
    TEST_ESP_OK(espos_config_register_ns(&s_rate_ns));
    doc = schema_doc(e2);
    TEST_ASSERT_TRUE(strcmp(e1, e2) != 0);
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "properties"), "f_rate"));
    cJSON_Delete(doc);
    TEST_ESP_OK(espos_config_unregister_ns("f_rate"));
    char e3[ESPOS_CFG_ETAG_MAX];
    doc = schema_doc(e3);
    TEST_ASSERT_TRUE(strcmp(e3, e2) != 0);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "properties"), "f_rate"));
    cJSON_Delete(doc);
    /* Back to nothing registered: the document is the compiled one again, so
     * the ETag returns to the compiled value and a browser that cached it
     * before any node existed keeps its 304. */
    TEST_ESP_OK(espos_config_unregister_ns("f_cal"));
    char e4[ESPOS_CFG_ETAG_MAX];
    doc = schema_doc(e4);
    TEST_ASSERT_EQUAL_STRING(espos_cfg_schema_etag, e4);
    cJSON_Delete(doc);
    fixture_teardown(m);
}

TEST_CASE("a runtime table key carries its format and columns into the schema", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_register_ns(&s_curve_ns));
    char etag[ESPOS_CFG_ETAG_MAX];
    cJSON *doc = schema_doc(etag);
    cJSON *p = cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "properties"), "f_curve"),
                            "properties"),
        "points");
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("table", cJSON_GetStringValue(cJSON_GetObjectItem(p, "x-espos-format")));
    cJSON *cols = cJSON_GetObjectItem(p, "x-espos-columns");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(cols));
    TEST_ASSERT_EQUAL_STRING("input", cJSON_GetStringValue(cJSON_GetArrayItem(cols, 0)));
    /* it stays a plain string in the schema, not a blob */
    TEST_ASSERT_EQUAL_STRING("string", cJSON_GetStringValue(cJSON_GetObjectItem(p, "type")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(p, "x-espos-type"));
    cJSON_Delete(doc);
    fixture_teardown(m);
}

/* ---------------------------------------------- static descriptor extensions */

TEST_CASE("readOnly, group and display hints reach the compiled tables", "[runtime]")
{
    const espos_cfg_ns_t *nd = espos_config_find_ns("disp");
    TEST_ASSERT_NOT_NULL(nd);
    const espos_cfg_key_t *serial = espos_config_find_key(nd, "serial");
    TEST_ASSERT_TRUE((serial->flags & ESPOS_CFG_FLAG_READ_ONLY) != 0);
    TEST_ASSERT_EQUAL_STRING("Device", serial->display.group);
    const espos_cfg_key_t *heading = espos_config_find_key(nd, "heading");
    TEST_ASSERT_EQUAL_FLOAT(57.29578f, heading->display.display_mul);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, heading->display.display_off);
    TEST_ASSERT_EQUAL_STRING("Calibration", heading->display.group);
    const espos_cfg_key_t *uptime = espos_config_find_key(nd, "uptime");
    TEST_ASSERT_EQUAL_FLOAT(0.5f, uptime->display.display_off);
    const espos_cfg_key_t *curve = espos_config_find_key(nd, "curve");
    TEST_ASSERT_EQUAL(2, curve->display.table_column_count);
    TEST_ASSERT_EQUAL_STRING("input", curve->display.table_columns[0]);
    TEST_ASSERT_EQUAL(3999, curve->max_len);
    /* a key that declares none of this is all-zero, i.e. identity */
    const espos_cfg_key_t *plain = espos_config_find_key(espos_config_find_ns(ESPOS_CFG_NS_T1),
                                                         ESPOS_CFG_T1_COUNT);
    TEST_ASSERT_NULL(plain->display.group);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, plain->display.display_mul);
    TEST_ASSERT_NULL(plain->display.table_columns);
}

#define SET_DISP "{\"disp\":{\"serial\":\"other\",\"heading\":0.25}}"

TEST_CASE("a read-only key is served but never written", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    char s[24];
    TEST_ESP_OK(espos_config_get_str("disp", "serial", s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("unset", s);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, espos_config_set_str("disp", "serial", "1234"));
    TEST_ESP_OK(espos_config_get_str("disp", "serial", s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("unset", s);

    /* An import carrying the whole document — what "restore my settings"
     * does — must not fail on it; the read-only member is simply ignored. */
    espos_config_import_result_t r;
    TEST_ESP_OK(espos_config_import_json(SET_DISP, strlen(SET_DISP), false, &r, NULL));
    TEST_ASSERT_EQUAL(1, r.changed);
    TEST_ESP_OK(espos_config_get_str("disp", "serial", s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("unset", s);
    float f = 0;
    TEST_ESP_OK(espos_config_get_float("disp", "heading", &f));
    TEST_ASSERT_EQUAL_FLOAT(0.25f, f);
    fixture_teardown(m);
}

TEST_CASE("static display hints reach the served schema too", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    char etag[ESPOS_CFG_ETAG_MAX];
    cJSON *doc = schema_doc(etag);
    cJSON *ps = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(doc, "properties"), "disp"),
                                    "properties");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetObjectItem(ps, "serial"), "readOnly")));
    TEST_ASSERT_EQUAL_STRING("Device",
                             cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(ps, "serial"),
                                                                      "x-espos-group")));
    cJSON *h = cJSON_GetObjectItem(ps, "heading");
    TEST_ASSERT_EQUAL_DOUBLE(57.29578, cJSON_GetNumberValue(cJSON_GetObjectItem(h, "x-espos-displayMultiplier")));
    cJSON *u = cJSON_GetObjectItem(ps, "uptime");
    TEST_ASSERT_EQUAL_DOUBLE(0.5, cJSON_GetNumberValue(cJSON_GetObjectItem(u, "x-espos-displayOffset")));
    cJSON *c = cJSON_GetObjectItem(ps, "curve");
    TEST_ASSERT_EQUAL_STRING("table", cJSON_GetStringValue(cJSON_GetObjectItem(c, "x-espos-format")));
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(cJSON_GetObjectItem(c, "x-espos-columns")));
    cJSON_Delete(doc);
    fixture_teardown(m);
}

/* ------------------------------------------------------------ NVS footprint */

/*
 * What a realistic graph costs in the 24K nvs partition. Eight nodes with
 * three parameters each plus one 250-point curve table, measured through the
 * real key/value shapes rather than guessed at. The numbers are printed so a
 * partition decision can be made from data; see docs/config.md.
 */
TEST_CASE("a realistic graph's NVS footprint is reported", "[runtime]")
{
    espos_config_mem_t *m = fixture_setup();
    static char names[9][8];
    static espos_cfg_ns_t graph[9];
    for (int i = 0; i < 8; i++) {
        snprintf(names[i], sizeof(names[i]), "f_n%02d", i);
        graph[i] = (espos_cfg_ns_t) { .name = names[i], .title = "Node", .version = 1, .keys = s_cal_keys, .key_count = 3 };
        TEST_ESP_OK(espos_config_register_ns(&graph[i]));
        TEST_ESP_OK(espos_config_set_float(names[i], "mul", 1.5f + i));
        TEST_ESP_OK(espos_config_set_float(names[i], "off", 0.25f * i));
        TEST_ESP_OK(espos_config_set_str(names[i], "label", "sensor"));
    }
    TEST_ESP_OK(espos_config_register_ns(&s_curve_ns));

    /* 250 points as "[[x,y],...]" — the shape the table editor writes. */
    char *curve = malloc(4000);
    TEST_ASSERT_NOT_NULL(curve);
    size_t n = 0;
    curve[n++] = '[';
    for (int i = 0; i < 250 && n < 3990; i++) {
        n += snprintf(curve + n, 4000 - n, "%s[%d,%d]", i ? "," : "", i, i * 2);
    }
    curve[n++] = ']';
    curve[n] = '\0';
    TEST_ASSERT_TRUE(n <= 3999);
    TEST_ESP_OK(espos_config_set_str("f_curve", "points", curve));

    /* Every namespace also carries its config_version stamp. */
    size_t entries = 0;
    for (int i = 0; i < 8; i++) {
        entries += espos_config_mem_key_count(m, names[i]);
    }
    entries += espos_config_mem_key_count(m, "f_curve");

    /* NVS packs into 32-byte entries: a scalar is one, a string/blob is one
     * header entry plus ceil(len/32) data entries, and a page holds 126 of
     * them in 4096 bytes. This mirrors nvs_page.hpp's layout. */
    size_t scalar_entries = 8 * 2 + 8 /* mul, off, config_version */ + 1 /* curve stamp */;
    size_t label_entries = 8 * (1 + (strlen("sensor") + 1 + 31) / 32);
    size_t curve_entries = 1 + (n + 1 + 31) / 32;
    size_t total_entries = scalar_entries + label_entries + curve_entries;
    size_t bytes = total_entries * 32;
    size_t pages = (total_entries + 125) / 126;

    printf("\nNVS footprint of 8 nodes x 3 params + one 250-point curve table:\n");
    printf("  namespaces      : 9 (8 nodes + 1 curve)\n");
    printf("  backend entries : %u keys stored\n", (unsigned)entries);
    printf("  curve string    : %u bytes\n", (unsigned)n);
    printf("  NVS entries     : %u (scalars %u, labels %u, curve %u)\n",
           (unsigned)total_entries, (unsigned)scalar_entries, (unsigned)label_entries,
           (unsigned)curve_entries);
    printf("  NVS data bytes  : %u\n", (unsigned)bytes);
    printf("  NVS pages       : %u of 12 in a 48K partition\n", (unsigned)pages);
    /* The claim docs/config.md makes: a graph of this size fits well inside
     * the 48K partition next to the built-in namespaces, with room for the
     * free page NVS compacts into and for rewrites that briefly hold both
     * the old and the new value. Three of twelve leaves that margin; three
     * of the six pages the partition had through v0.7 did not. */
    TEST_ASSERT_TRUE_MESSAGE(pages <= 3, "a realistic graph must not fill the nvs partition");
    free(curve);
    fixture_teardown(m);
}
