/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "test_common.h"

/* Namespace "mig" is at version 3. Simulated history:
 *   v1: key "speed" (int, seconds)
 *   v2: renamed to "speed_ms" (int, milliseconds)   ← migration 1→2 below
 *   v3: added "label" (additive, no migration needed)
 */
/* Callbacks run under the store lock: never TEST_ASSERT inside them (a
 * failing assert would longjmp out with the mutex held and wedge every later
 * test). Record observations, assert after init returns. */
static int s_calls_1to2, s_calls_2to3;
static uint16_t s_seen_from;
static void *s_seen_arg;
static esp_err_t s_set_err, s_erase_err, s_public_api_err;

static esp_err_t mig_1_to_2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    s_calls_1to2++;
    s_seen_from = espos_config_migrate_from_version(ctx);
    s_seen_arg = arg;
    int32_t seconds = 0;
    size_t len = sizeof(seconds);
    esp_err_t err = espos_config_migrate_get(ctx, "speed", ESPOS_CFG_TYPE_INT, &seconds, &len);
    if (err == ESP_OK) {
        int32_t ms = seconds * 1000;
        s_set_err = espos_config_migrate_set(ctx, "speed_ms", ESPOS_CFG_TYPE_INT, &ms, sizeof(ms));
        s_erase_err = espos_config_migrate_erase(ctx, "speed");
    }
    return ESP_OK;
}

static esp_err_t mig_fail(espos_config_migrate_ctx_t *ctx, void *arg)
{
    (void)ctx;
    (void)arg;
    s_calls_2to3++;
    /* The public API is not usable from inside a migration (the store is
     * not initialised yet); it must refuse rather than deadlock. */
    int32_t v;
    s_public_api_err = espos_config_get_i32(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_SPEED_MS, &v);
    return ESP_FAIL;
}

static esp_err_t mig_noop(espos_config_migrate_ctx_t *ctx, void *arg)
{
    (void)ctx;
    (void)arg;
    return ESP_OK;
}

static void plant_version(espos_config_mem_t *m, const char *ns, int32_t v)
{
    TEST_ESP_OK(espos_config_mem_plant(m, ns, ESPOS_CFG_VERSION_KEY, ESPOS_CFG_TYPE_INT, &v, 4));
}

TEST_CASE("fresh store is stamped with the current version, no migrations run", "[migration]")
{
    s_calls_1to2 = 0;
    espos_config_mem_t *m = espos_config_mem_create();
    TEST_ESP_OK(espos_config_register_migration("mig", 1, mig_1_to_2, (void *)0x1234));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    uint16_t stored = 0, cur = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, &cur));
    TEST_ASSERT_EQUAL(3, stored);
    TEST_ASSERT_EQUAL(3, cur);
    TEST_ASSERT_EQUAL(0, s_calls_1to2);
    TEST_ASSERT_TRUE(espos_config_mem_has_key(m, "mig", ESPOS_CFG_VERSION_KEY));
    fixture_teardown(m);
}

TEST_CASE("upgrade chain runs registered steps and treats missing steps as additive", "[migration]")
{
    s_calls_1to2 = 0;
    espos_config_mem_t *m = espos_config_mem_create();
    plant_version(m, "mig", 1);
    int32_t seconds = 3;
    TEST_ESP_OK(espos_config_mem_plant(m, "mig", "speed", ESPOS_CFG_TYPE_INT, &seconds, 4));
    TEST_ESP_OK(espos_config_register_migration("mig", 1, mig_1_to_2, (void *)0x1234));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    TEST_ASSERT_EQUAL(1, s_calls_1to2);
    TEST_ASSERT_EQUAL(1, s_seen_from);
    TEST_ASSERT_EQUAL_PTR((void *)0x1234, s_seen_arg);
    TEST_ESP_OK(s_set_err);
    TEST_ESP_OK(s_erase_err);
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, NULL));
    TEST_ASSERT_EQUAL(3, stored);
    int32_t ms = 0;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_SPEED_MS, &ms));
    TEST_ASSERT_EQUAL_INT32(3000, ms);
    TEST_ASSERT_FALSE(espos_config_mem_has_key(m, "mig", "speed"));
    /* second boot: nothing to do */
    espos_config_deinit();
    TEST_ESP_OK(espos_config_register_migration("mig", 1, mig_1_to_2, (void *)0x1234));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    TEST_ASSERT_EQUAL(1, s_calls_1to2);
    fixture_teardown(m);
}

TEST_CASE("a failing migration keeps the stored version and does not run later steps", "[migration]")
{
    s_calls_1to2 = 0;
    s_calls_2to3 = 0;
    espos_config_mem_t *m = espos_config_mem_create();
    plant_version(m, "mig", 1);
    TEST_ESP_OK(espos_config_register_migration("mig", 1, mig_1_to_2, (void *)0x1234));
    TEST_ESP_OK(espos_config_register_migration("mig", 2, mig_fail, NULL));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m)); /* init still succeeds */
    TEST_ASSERT_EQUAL(1, s_calls_1to2);
    TEST_ASSERT_EQUAL(1, s_calls_2to3);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, s_public_api_err);
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, NULL));
    TEST_ASSERT_EQUAL(2, stored); /* 1→2 committed, 2→3 failed */
    /* store is still usable, defaults served */
    int32_t ms = 0;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_SPEED_MS, &ms));
    TEST_ASSERT_EQUAL_INT32(500, ms);
    /* next boot retries the failed step */
    espos_config_deinit();
    TEST_ESP_OK(espos_config_register_migration("mig", 2, mig_fail, NULL));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    TEST_ASSERT_EQUAL(2, s_calls_2to3);
    fixture_teardown(m);
}

TEST_CASE("downgrade: newer stored version is left alone", "[migration]")
{
    espos_config_mem_t *m = espos_config_mem_create();
    plant_version(m, "mig", 7);
    int32_t future = 42;
    TEST_ESP_OK(espos_config_mem_plant(m, "mig", "speed_ms", ESPOS_CFG_TYPE_INT, &future, 4));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, NULL));
    TEST_ASSERT_EQUAL(7, stored);
    int32_t ms;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_SPEED_MS, &ms));
    TEST_ASSERT_EQUAL_INT32(42, ms); /* still readable */
    fixture_teardown(m);
}

TEST_CASE("corrupt version stamp is replaced", "[migration]")
{
    espos_config_mem_t *m = espos_config_mem_create();
    TEST_ESP_OK(espos_config_mem_plant(m, "mig", ESPOS_CFG_VERSION_KEY, ESPOS_CFG_TYPE_STRING, "v1", 0));
    plant_version(m, "t1", -3); /* negative → invalid */
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, NULL));
    TEST_ASSERT_EQUAL(3, stored);
    TEST_ESP_OK(espos_config_get_version("t1", &stored, NULL));
    TEST_ASSERT_EQUAL(1, stored);
    fixture_teardown(m);
}

TEST_CASE("migration registration rules", "[migration]")
{
    espos_config_mem_t *m = espos_config_mem_create();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_migration("nope", 1, mig_1_to_2, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_migration("mig", 3, mig_1_to_2, NULL)); /* >= current */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_migration("mig", 1, NULL, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_migration("mig", 0, mig_1_to_2, NULL)); /* versions start at 1 */
    TEST_ESP_OK(espos_config_register_migration("mig", 1, mig_1_to_2, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_register_migration("mig", 1, mig_fail, NULL)); /* dup */
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, espos_config_register_migration("mig", 2, mig_fail, NULL)); /* after init */
    fixture_teardown(m);
    /* table full (CONFIG_ESPOS_CONFIG_MAX_MIGRATIONS=8 in this test app) */
    m = espos_config_mem_create();
    for (int i = 1; i <= 8; i++) {
        TEST_ESP_OK(espos_config_register_migration("big", (uint16_t)i, mig_noop, NULL));
    }
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, espos_config_register_migration("big", 9, mig_noop, NULL));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_config_register_migration("t1", 1, mig_1_to_2, NULL)); /* t1 is v1 */
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    fixture_teardown(m);
}

TEST_CASE("long upgrade chain with a mix of registered and additive steps", "[migration]")
{
    espos_config_mem_t *m = espos_config_mem_create();
    plant_version(m, "big", 5);
    TEST_ESP_OK(espos_config_register_migration("big", 7, mig_noop, NULL));
    TEST_ESP_OK(espos_config_register_migration("big", 19, mig_noop, NULL));
    TEST_ESP_OK(espos_config_init(espos_config_backend_mem(), m));
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("big", &stored, NULL));
    TEST_ASSERT_EQUAL(20, stored);
    fixture_teardown(m);
}
