/* SPDX-License-Identifier: Apache-2.0 */
#include <stdlib.h>
#include "cJSON.h"
#include "test_common.h"

static cJSON *export_parsed(const char *ns)
{
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json(ns, false, &txt));
    TEST_ASSERT_NOT_NULL(txt);
    cJSON *j = cJSON_Parse(txt);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, txt);
    free(txt);
    return j;
}

static const cJSON *get2(const cJSON *root, const char *ns, const char *key)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, ns);
    TEST_ASSERT_NOT_NULL(n);
    return cJSON_GetObjectItemCaseSensitive(n, key);
}

TEST_CASE("export has every namespace and key with effective values", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    cJSON *j = export_parsed(NULL);
    TEST_ASSERT_TRUE(cJSON_IsObject(j));
    TEST_ASSERT_EQUAL(espos_cfg_namespace_count, cJSON_GetArraySize(j));
    TEST_ASSERT_TRUE(cJSON_IsTrue(get2(j, "t1", "flag")));
    TEST_ASSERT_EQUAL_DOUBLE(7, cJSON_GetNumberValue(get2(j, "t1", "count")));
    TEST_ASSERT_EQUAL_DOUBLE(0, cJSON_GetNumberValue(get2(j, "t1", "any_int")));
    TEST_ASSERT_EQUAL_STRING("hello", cJSON_GetStringValue(get2(j, "t1", "name")));
    TEST_ASSERT_EQUAL_STRING("b", cJSON_GetStringValue(get2(j, "t1", "mode")));
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(get2(j, "t1", "blob")));
    TEST_ASSERT_EQUAL(8, cJSON_GetArraySize(cJSON_GetObjectItem(j, "t1")));
    /* config_version is internal and never exported */
    TEST_ASSERT_NULL(get2(j, "t1", "config_version"));
    cJSON_Delete(j);

    /* single namespace filter */
    j = export_parsed("mig");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(j));
    TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(j, "mig"));
    cJSON_Delete(j);
    char *txt = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_export_json("nope", false, &txt));
    TEST_ASSERT_NULL(txt);
    fixture_teardown(m);
}

TEST_CASE("export renders floats and blobs canonically", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json("t1", false, &txt));
    /* 0.1f must not come out as 0.10000000149011612 */
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "\"ratio\":0.1,"), txt);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(txt, "\"any_float\":-1.5"), txt);
    free(txt);
    const uint8_t data[] = { 'f', 'o', 'o', 'b' };
    TEST_ESP_OK(espos_config_set_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, data, 4));
    cJSON *j = export_parsed("t1");
    TEST_ASSERT_EQUAL_STRING("Zm9vYg==", cJSON_GetStringValue(get2(j, "t1", "blob")));
    cJSON_Delete(j);
    fixture_teardown(m);
}

TEST_CASE("secrets are redacted on export and the sentinel is ignored on import", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    /* unset secret exports as empty string, not the sentinel */
    cJSON *j = export_parsed("sec");
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(get2(j, "sec", "password")));
    TEST_ASSERT_EQUAL_STRING("", cJSON_GetStringValue(get2(j, "sec", "key_blob")));
    TEST_ASSERT_EQUAL_STRING("pub", cJSON_GetStringValue(get2(j, "sec", "public")));
    cJSON_Delete(j);

    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_SEC, ESPOS_CFG_SEC_PASSWORD, "hunter2"));
    const uint8_t kb[3] = { 1, 2, 3 };
    TEST_ESP_OK(espos_config_set_blob(ESPOS_CFG_NS_SEC, ESPOS_CFG_SEC_KEY_BLOB, kb, 3));
    j = export_parsed("sec");
    TEST_ASSERT_EQUAL_STRING(ESPOS_CONFIG_SECRET_SENTINEL, cJSON_GetStringValue(get2(j, "sec", "password")));
    TEST_ASSERT_EQUAL_STRING(ESPOS_CONFIG_SECRET_SENTINEL, cJSON_GetStringValue(get2(j, "sec", "key_blob")));
    cJSON_Delete(j);

    /* include_secrets=true reveals them (used only for trusted paths) */
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json("sec", true, &txt));
    TEST_ASSERT_NOT_NULL(strstr(txt, "\"password\":\"hunter2\""));
    TEST_ASSERT_NOT_NULL(strstr(txt, "\"key_blob\":\"AQID\""));
    free(txt);

    /* Round-trip the redacted export back in: secrets untouched, others applied */
    const char *doc = "{\"sec\":{\"password\":\"********\",\"key_blob\":\"********\",\"public\":\"changed\"}}";
    espos_config_import_result_t r;
    char *rep = NULL;
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, &rep));
    TEST_ASSERT_EQUAL(1, r.changed);
    TEST_ASSERT_EQUAL_STRING("{\"changed\":[\"sec.public\"],\"restart_required\":false}", rep);
    free(rep);
    char s[33];
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_SEC, ESPOS_CFG_SEC_PASSWORD, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("hunter2", s);
    /* a real value still overwrites */
    doc = "{\"sec\":{\"password\":\"newpw\"}}";
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, NULL));
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_SEC, ESPOS_CFG_SEC_PASSWORD, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("newpw", s);
    /* and null resets a secret */
    doc = "{\"sec\":{\"password\":null}}";
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, NULL));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_SEC, ESPOS_CFG_SEC_PASSWORD));
    fixture_teardown(m);
}

TEST_CASE("import applies a full document and reports changes", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));
    const char *doc =
        "{\"t1\":{\"flag\":false,\"count\":-5,\"any_int\":123456,\"ratio\":2.5,\"any_float\":1e3,"
        "\"name\":\"abc\",\"mode\":\"a\",\"blob\":\"AQID\"},"
        "\"mig\":{\"speed_ms\":250,\"label\":\"x\"}}";
    espos_config_import_result_t r;
    char *rep = NULL;
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, &rep));
    TEST_ASSERT_EQUAL(9, r.changed); /* mig.label == default → not a change */
    TEST_ASSERT_TRUE(r.restart_required); /* t1.count is restart_required */
    TEST_ASSERT_EQUAL(9, rec.count);
    TEST_ASSERT_TRUE(rec_has(&rec, "t1.blob"));
    TEST_ASSERT_TRUE(rec_has(&rec, "mig.speed_ms"));
    TEST_ASSERT_FALSE(rec_has(&rec, "mig.label"));
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"restart_required\":true"));
    TEST_ASSERT_NOT_NULL(strstr(rep, "\"t1.count\""));
    TEST_ASSERT_NULL(strstr(rep, "\"mig.label\""));
    free(rep);

    bool b;
    int32_t i;
    float f;
    char s[16];
    uint8_t blob[8];
    size_t blen = sizeof(blob);
    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_FALSE(b);
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(-5, i);
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_FLOAT, &f));
    TEST_ASSERT_EQUAL_FLOAT(1000.0f, f);
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("a", s);
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, &blen));
    TEST_ASSERT_EQUAL(3, blen);
    TEST_ASSERT_EQUAL_UINT8(3, blob[2]);

    /* export → import round trip is a no-op */
    char *txt = NULL;
    TEST_ESP_OK(espos_config_export_json(NULL, false, &txt));
    rec.count = 0;
    TEST_ESP_OK(espos_config_import_json(txt, strlen(txt), false, &r, NULL));
    TEST_ASSERT_EQUAL(0, r.changed);
    TEST_ASSERT_EQUAL(0, rec.count);
    free(txt);

    /* partial document, null resets */
    doc = "{\"t1\":{\"count\":null,\"blob\":\"\"}}";
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, NULL));
    TEST_ASSERT_EQUAL(2, r.changed);
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB));
    TEST_ASSERT_TRUE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME)); /* untouched */
    fixture_teardown(m);
}

static void expect_reject(const char *doc, const char *path, const char *msg_substr)
{
    espos_config_import_result_t r;
    char *rep = NULL;
    TEST_ASSERT_EQUAL_MESSAGE(ESP_ERR_INVALID_ARG,
                              espos_config_import_json(doc, strlen(doc), false, &r, &rep), doc);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(path, r.error_path, doc);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(r.error_msg, msg_substr), r.error_msg);
    TEST_ASSERT_NOT_NULL(rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rep, "\"error\":\"validation\""), rep);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(rep, path), rep);
    free(rep);
}

TEST_CASE("import rejects invalid documents and writes nothing", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));

    expect_reject("nope", "", "malformed");
    expect_reject("[1,2]", "", "expected object");
    expect_reject("{\"t1\":5}", "t1", "expected object");
    expect_reject("{\"zzz\":{}}", "zzz", "unknown namespace");
    expect_reject("{\"t1\":{\"zzz\":1}}", "t1.zzz", "unknown key");
    expect_reject("{\"t1\":{\"flag\":1}}", "t1.flag", "expected boolean");
    expect_reject("{\"t1\":{\"count\":\"7\"}}", "t1.count", "expected integer");
    expect_reject("{\"t1\":{\"count\":7.5}}", "t1.count", "expected 32-bit integer");
    expect_reject("{\"t1\":{\"any_int\":3000000000}}", "t1.any_int", "expected 32-bit integer");
    expect_reject("{\"t1\":{\"count\":1001}}", "t1.count", "out of range");
    expect_reject("{\"t1\":{\"ratio\":true}}", "t1.ratio", "expected number");
    expect_reject("{\"t1\":{\"ratio\":10.01}}", "t1.ratio", "above maximum");
    expect_reject("{\"t1\":{\"name\":42}}", "t1.name", "expected string");
    expect_reject("{\"t1\":{\"name\":\"123456789\"}}", "t1.name", "longer than 8");
    expect_reject("{\"t1\":{\"mode\":\"zzz\"}}", "t1.mode", "not an allowed value");
    expect_reject("{\"t1\":{\"mode\":\"nope\"}}", "t1.mode", "longer than 3"); /* enum maxLength = longest value */
    expect_reject("{\"t1\":{\"blob\":\"!!!\"}}", "t1.blob", "invalid base64");
    expect_reject("{\"t1\":{\"blob\":\"AAAAAAAAAAAAAAAA\"}}", "t1.blob", "longer than 8");
    expect_reject("{\"t1\":{\"blob\":7}}", "t1.blob", "expected base64");
    /* a valid key before the invalid one must NOT be written (all-or-nothing) */
    expect_reject("{\"t1\":{\"count\":9,\"name\":\"ok\"},\"mig\":{\"speed_ms\":-1}}", "mig.speed_ms", "out of range");
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME));
    TEST_ASSERT_EQUAL(0, rec.count);

    /* ignore_unknown skips foreign namespaces/keys but still validates the rest */
    espos_config_import_result_t r;
    const char *doc = "{\"zzz\":{\"a\":1},\"t1\":{\"zzz\":1,\"count\":9}}";
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), true, &r, NULL));
    TEST_ASSERT_EQUAL(1, r.changed);
    doc = "{\"zzz\":{\"a\":1},\"t1\":{\"count\":99999}}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json(doc, strlen(doc), true, &r, NULL));
    fixture_teardown(m);
}

TEST_CASE("import: duplicate keys — last one wins", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    espos_config_import_result_t r;
    const char *doc = "{\"t1\":{\"count\":1,\"count\":2,\"blob\":\"AQ==\",\"blob\":\"Ag==\"}}";
    TEST_ESP_OK(espos_config_import_json(doc, strlen(doc), false, &r, NULL));
    TEST_ASSERT_EQUAL(2, r.changed);
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(2, i);
    uint8_t b[8];
    size_t n = sizeof(b);
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, b, &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL_UINT8(2, b[0]);
    fixture_teardown(m);
}

TEST_CASE("import: storage failure is reported, not hidden", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    espos_config_mem_set_write_fault(m, ESP_ERR_NO_MEM);
    espos_config_import_result_t r;
    char *rep = NULL;
    const char *doc = "{\"t1\":{\"count\":9}}";
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_config_import_json(doc, strlen(doc), false, &r, &rep));
    TEST_ASSERT_EQUAL(0, r.changed);
    TEST_ASSERT_NULL(rep); /* no success report on failure */
    espos_config_mem_set_write_fault(m, ESP_OK);
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    fixture_teardown(m);
}

TEST_CASE("import: empty and no-op documents", "[json]")
{
    espos_config_mem_t *m = fixture_setup();
    espos_config_import_result_t r;
    char *rep = NULL;
    TEST_ESP_OK(espos_config_import_json("{}", 2, false, &r, &rep));
    TEST_ASSERT_EQUAL(0, r.changed);
    TEST_ASSERT_EQUAL_STRING("{\"changed\":[],\"restart_required\":false}", rep);
    free(rep);
    TEST_ESP_OK(espos_config_import_json("{\"t1\":{}}", 9, false, &r, NULL));
    TEST_ASSERT_EQUAL(0, r.changed);
    /* explicit length shorter than the string is honoured */
    TEST_ESP_OK(espos_config_import_json("{}garbage", 2, false, &r, NULL));
    /* trailing non-whitespace after the document is malformed */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json("{}garbage", 9, false, &r, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json("{}{}", 4, false, &r, NULL));
    TEST_ESP_OK(espos_config_import_json("{} \n\t", 5, false, &r, NULL));
    /* NUL inside a string (raw or escaped) would silently truncate → malformed */
    const char raw_nul[] = "{\"t1\":{\"name\":\"ab\0cdefghijklmnop\"}}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json(raw_nul, sizeof(raw_nul) - 1, false, &r, NULL));
    const char *esc_nul = "{\"t1\":{\"name\":\"ab\\u0000cdefghijklmnop\"}}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json(esc_nul, strlen(esc_nul), false, &r, NULL));
    const char *esc_key = "{\"t1\":{\"count\\u0000x\":1}}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_import_json(esc_key, strlen(esc_key), false, &r, NULL));
    /* a literal backslash followed by u0000 is fine ("\\u0000" in JSON) */
    const char *lit = "{\"t1\":{\"name\":\"a\\\\u000\"}}";
    TEST_ESP_OK(espos_config_import_json(lit, strlen(lit), false, &r, NULL));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    fixture_teardown(m);
}

TEST_CASE("export/import refuse before init", "[json]")
{
    char *txt = NULL;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_export_json(NULL, false, &txt));
    TEST_ASSERT_NULL(txt);
    espos_config_import_result_t r;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_import_json("{\"t1\":{\"count\":1}}", 18, false, &r, NULL));
}

TEST_CASE("schema JSON parses and describes every declared key", "[json]")
{
    cJSON *schema = cJSON_ParseWithLength(espos_cfg_schema_json, espos_cfg_schema_json_len);
    TEST_ASSERT_NOT_NULL(schema);
    cJSON *props = cJSON_GetObjectItem(schema, "properties");
    TEST_ASSERT_NOT_NULL(props);
    for (size_t i = 0; i < espos_cfg_namespace_count; i++) {
        const espos_cfg_ns_t *ns = &espos_cfg_namespaces[i];
        cJSON *jns = cJSON_GetObjectItem(props, ns->name);
        TEST_ASSERT_NOT_NULL_MESSAGE(jns, ns->name);
        TEST_ASSERT_EQUAL(ns->version, (int)cJSON_GetNumberValue(cJSON_GetObjectItem(jns, "x-espos-version")));
        cJSON *jkeys = cJSON_GetObjectItem(jns, "properties");
        TEST_ASSERT_EQUAL(ns->key_count, cJSON_GetArraySize(jkeys));
        for (size_t k = 0; k < ns->key_count; k++) {
            cJSON *jk = cJSON_GetObjectItem(jkeys, ns->keys[k].name);
            TEST_ASSERT_NOT_NULL_MESSAGE(jk, ns->keys[k].name);
            TEST_ASSERT_NOT_NULL(cJSON_GetObjectItem(jk, "type"));
        }
    }
    /* secret + enum + restart flags surfaced */
    cJSON *pw = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "sec"), "properties"), "password");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(pw, "writeOnly")));
    cJSON *cnt = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "t1"), "properties"), "count");
    TEST_ASSERT_TRUE(cJSON_IsTrue(cJSON_GetObjectItem(cnt, "x-espos-restartRequired")));
    TEST_ASSERT_EQUAL_DOUBLE(-100, cJSON_GetNumberValue(cJSON_GetObjectItem(cnt, "minimum")));
    cJSON *mode = cJSON_GetObjectItem(cJSON_GetObjectItem(cJSON_GetObjectItem(props, "t1"), "properties"), "mode");
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(cJSON_GetObjectItem(mode, "enum")));
    TEST_ASSERT_EQUAL_DOUBLE(3, cJSON_GetNumberValue(cJSON_GetObjectItem(mode, "maxLength")));
    cJSON_Delete(schema);
}
