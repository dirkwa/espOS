/* SPDX-License-Identifier: Apache-2.0 */
#include <math.h>
#include "test_common.h"

TEST_CASE("defaults are served from an empty store", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    bool b = false;
    int32_t i = 0;
    float f = 0;
    char s[16];
    size_t n = 0;
    uint8_t blob[8];
    size_t blen = sizeof(blob);

    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_TRUE(b);
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_INT, &i));
    TEST_ASSERT_EQUAL_INT32(0, i);
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, &f));
    TEST_ASSERT_EQUAL_FLOAT(0.1f, f);
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_FLOAT, &f));
    TEST_ASSERT_EQUAL_FLOAT(-1.5f, f);
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), &n));
    TEST_ASSERT_EQUAL_STRING("hello", s);
    TEST_ASSERT_EQUAL(5, n);
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("b", s);
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, &blen));
    TEST_ASSERT_EQUAL(0, blen);
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    fixture_teardown(m);
}

TEST_CASE("undeclared namespace/key and wrong-type access are rejected", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    bool b;
    int32_t i;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_get_bool("nope", "flag", &b));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_get_bool(ESPOS_CFG_NS_T1, "nope", &b));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_set_i32(ESPOS_CFG_NS_T1, "nope", 1));
    /* type confusion: reading an int key as bool */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &i));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, "x"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, NULL));
    fixture_teardown(m);
}

TEST_CASE("set/get round trip for every type", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    bool b = true;
    int32_t i = 0;
    float f = 0;
    char s[16];
    uint8_t blob[8];
    size_t blen;

    TEST_ESP_OK(espos_config_set_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, false));
    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_FALSE(b);
    TEST_ASSERT_TRUE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG));

    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, -100));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(-100, i);
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_INT, INT32_MIN));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_INT, &i));
    TEST_ASSERT_EQUAL_INT32(INT32_MIN, i);

    TEST_ESP_OK(espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, 3.25f));
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, &f));
    TEST_ASSERT_EQUAL_FLOAT(3.25f, f);

    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "12345678")); /* exactly maxLength */
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("12345678", s);
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "")); /* empty is a value, not a reset */
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("", s);
    TEST_ASSERT_TRUE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME));

    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, "ccc"));
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("ccc", s);

    const uint8_t data[8] = { 0, 1, 2, 3, 0xff, 0xfe, 0x80, 0x7f };
    TEST_ESP_OK(espos_config_set_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, data, sizeof(data)));
    blen = 0;
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, NULL, &blen)); /* size query */
    TEST_ASSERT_EQUAL(8, blen);
    blen = sizeof(blob);
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, &blen));
    TEST_ASSERT_EQUAL(8, blen);
    TEST_ASSERT_EQUAL_MEMORY(data, blob, 8);
    blen = 4; /* too small */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, &blen));
    TEST_ASSERT_EQUAL(8, blen);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, NULL));
    /* empty blob == erased */
    TEST_ESP_OK(espos_config_set_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, NULL, 0));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB));
    fixture_teardown(m);
}

TEST_CASE("writes are validated against the descriptor", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 1001));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, -101));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, 10.5f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, -0.001f));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_FLOAT, NAN));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_FLOAT, INFINITY));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "123456789"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, "d"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, "A"));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, NULL));
    uint8_t nine[9] = { 0 };
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, nine, 9));
    /* nothing was written */
    TEST_ASSERT_EQUAL(0, espos_config_mem_key_count(m, "t1") - 1 /* config_version */);
    fixture_teardown(m);
}

TEST_CASE("corrupt stored values fall back to defaults", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    int32_t i;
    float f;
    char s[16];
    bool b;
    /* wrong type in storage */
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "count", ESPOS_CFG_TYPE_STRING, "oops", 0));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    /* out of range in storage (older firmware had wider limits) */
    int32_t big = 5000;
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "count", ESPOS_CFG_TYPE_INT, &big, 4));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    /* string longer than maxLength in storage */
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "name", ESPOS_CFG_TYPE_STRING, "waytoolongvalue", 0));
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("hello", s);
    /* enum violation in storage */
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "mode", ESPOS_CFG_TYPE_STRING, "zzz", 0));
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_MODE, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("b", s);
    /* NaN in storage */
    float nan = NAN;
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "ratio", ESPOS_CFG_TYPE_FLOAT, &nan, 4));
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, &f));
    TEST_ASSERT_EQUAL_FLOAT(0.1f, f);
    /* bool with garbage byte */
    uint8_t seven = 7;
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "flag", ESPOS_CFG_TYPE_BOOL, &seven, 1));
    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_TRUE(b); /* default */
    /* oversized blob in storage */
    uint8_t twelve[12] = { 1 };
    TEST_ESP_OK(espos_config_mem_plant(m, "t1", "blob", ESPOS_CFG_TYPE_BLOB, twelve, 12));
    size_t blen = 0;
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, NULL, &blen));
    TEST_ASSERT_EQUAL(0, blen);
    fixture_teardown(m);
}

TEST_CASE("get_str reports truncation", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    char s[4];
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), &n));
    TEST_ASSERT_EQUAL_STRING("hel", s);
    TEST_ASSERT_EQUAL(5, n);
    /* size query only */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, NULL, 0, &n));
    TEST_ASSERT_EQUAL(5, n);
    fixture_teardown(m);
}

TEST_CASE("reset_key and reset_ns restore defaults", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    int32_t i;
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    TEST_ESP_OK(espos_config_set_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, false));
    TEST_ESP_OK(espos_config_reset_key(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT));
    TEST_ASSERT_TRUE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG));
    TEST_ESP_OK(espos_config_reset_key(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT)); /* idempotent */
    TEST_ESP_OK(espos_config_reset_ns(ESPOS_CFG_NS_T1));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG));
    /* version stamp survives a namespace reset */
    uint16_t stored = 0, cur = 0;
    TEST_ESP_OK(espos_config_get_version(ESPOS_CFG_NS_T1, &stored, &cur));
    TEST_ASSERT_EQUAL(1, stored);
    TEST_ASSERT_EQUAL(1, cur);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_reset_ns("nope"));
    fixture_teardown(m);
}

TEST_CASE("reset_ns notifies exactly the keys that changed", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "hello")); /* stored == default */
    TEST_ESP_OK(espos_config_set_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, false));
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));
    TEST_ESP_OK(espos_config_reset_ns(ESPOS_CFG_NS_T1));
    TEST_ASSERT_EQUAL(2, rec.count); /* count and flag changed; name was already at its default */
    TEST_ASSERT_TRUE(rec_has(&rec, "t1.count"));
    TEST_ASSERT_TRUE(rec_has(&rec, "t1.flag"));
    TEST_ASSERT_FALSE(rec_has(&rec, "t1.name"));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME));
    fixture_teardown(m);
}

TEST_CASE("change callbacks may re-enter the store", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    /* a callback that reads and writes another key must not deadlock */
    extern void reentrant_cb(const char *ns, const char *key, void *arg);
    int hits = 0;
    TEST_ESP_OK(espos_config_subscribe(reentrant_cb, &hits));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 5));
    int32_t v = 0;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_ANY_INT, &v));
    TEST_ASSERT_EQUAL_INT32(5, v); /* mirrored by the callback */
    TEST_ASSERT_EQUAL(2, hits);    /* count, then any_int (written by the callback itself) */
    fixture_teardown(m);
}

void reentrant_cb(const char *ns, const char *key, void *arg)
{
    int *hits = arg;
    (*hits)++;
    if (strcmp(key, ESPOS_CFG_T1_COUNT) == 0) {
        int32_t v = 0;
        espos_config_get_i32(ns, key, &v);
        espos_config_set_i32(ns, ESPOS_CFG_T1_ANY_INT, v);
    }
}

TEST_CASE("change notifications fire only on effective change", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 42)); /* == default → no change */
    TEST_ASSERT_EQUAL(0, rec.count);
    TEST_ASSERT_TRUE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT)); /* but stored */
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 43));
    TEST_ASSERT_EQUAL(1, rec.count);
    TEST_ASSERT_TRUE(rec_has(&rec, "t1.count"));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 43)); /* same again */
    TEST_ASSERT_EQUAL(1, rec.count);
    TEST_ESP_OK(espos_config_reset_key(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT)); /* 43 → 42 */
    TEST_ASSERT_EQUAL(2, rec.count);
    TEST_ESP_OK(espos_config_reset_key(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT)); /* already default */
    TEST_ASSERT_EQUAL(2, rec.count);
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "hello")); /* == default */
    TEST_ASSERT_EQUAL(2, rec.count);
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "bye"));
    TEST_ASSERT_EQUAL(3, rec.count);
    /* rejected writes never notify */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 99999));
    TEST_ASSERT_EQUAL(3, rec.count);
    /* unsubscribe */
    TEST_ESP_OK(espos_config_unsubscribe(rec_cb, &rec));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 44));
    TEST_ASSERT_EQUAL(3, rec.count);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_config_unsubscribe(rec_cb, &rec));
    /* table full */
    change_rec_t r2, r3, r4, r5;
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &r2));
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &r3));
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &r4));
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &r5));
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_config_subscribe(rec_cb, &rec));
    fixture_teardown(m);
}

TEST_CASE("write failure surfaces and leaves the effective value unchanged", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    change_rec_t rec = { 0 };
    TEST_ESP_OK(espos_config_subscribe(rec_cb, &rec));
    espos_config_mem_set_write_fault(m, ESP_ERR_NO_MEM);
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    TEST_ASSERT_EQUAL(0, rec.count);
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    espos_config_mem_set_write_fault(m, ESP_OK);
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    TEST_ASSERT_EQUAL(1, rec.count);
    fixture_teardown(m);
}

TEST_CASE("factory reset wipes values and re-stamps versions", "[core]")
{
    espos_config_mem_t *m = fixture_setup();
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 7));
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_LABEL, "y"));
    TEST_ESP_OK(espos_config_factory_reset());
    int32_t i;
    char s[8];
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_LABEL, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("x", s);
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version(ESPOS_CFG_NS_MIG, &stored, NULL));
    TEST_ASSERT_EQUAL(3, stored);
    /* store still usable */
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 8));
    TEST_ASSERT_FALSE(espos_config_storage_was_reset());
    fixture_teardown(m);
}

TEST_CASE("API refuses use before init and double init", "[core]")
{
    bool b = false;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_set_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_factory_reset());
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_reset_key(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_reset_ns(ESPOS_CFG_NS_T1));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_subscribe(rec_cb, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_get_version(ESPOS_CFG_NS_T1, NULL, NULL));
    TEST_ASSERT_FALSE(espos_config_is_set(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG));
    size_t n = 0;
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, NULL, &n));
    espos_config_mem_t *m = fixture_setup();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_init(espos_config_backend_mem(), m));
    fixture_teardown(m);
    /* re-init after deinit works */
    m = fixture_setup();
    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    fixture_teardown(m);
}

TEST_CASE("descriptor lookup helpers", "[core]")
{
    const espos_cfg_ns_t *ns = espos_config_find_ns(ESPOS_CFG_NS_T1);
    TEST_ASSERT_NOT_NULL(ns);
    TEST_ASSERT_EQUAL(8, ns->key_count);
    const espos_cfg_key_t *k = espos_config_find_key(ns, ESPOS_CFG_T1_COUNT);
    TEST_ASSERT_NOT_NULL(k);
    TEST_ASSERT_EQUAL(ESPOS_CFG_TYPE_INT, k->type);
    TEST_ASSERT_EQUAL(ESPOS_CFG_FLAG_RESTART_REQUIRED, k->flags);
    TEST_ASSERT_NULL(espos_config_find_key(ns, "nope"));
    TEST_ASSERT_NULL(espos_config_find_ns(NULL));
    TEST_ASSERT_TRUE(espos_cfg_namespace_count >= 3);
    TEST_ASSERT_TRUE(espos_cfg_schema_json_len > 100);
    TEST_ASSERT_EQUAL(16, strlen(espos_cfg_schema_etag));
    TEST_ASSERT_EQUAL_STRING(ESPOS_CFG_SCHEMA_ETAG, espos_cfg_schema_etag);
}
