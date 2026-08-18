/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * The real NVS backend on the host, on top of IDF's file-backed flash
 * emulation (partition table from partitions.csv in this test app).
 */
#include <stdlib.h>
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "nvs_flash.h"
#include "test_common.h"

static void nvs_fixture_wipe(void)
{
    /* Start every test from erased flash. */
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(nvs_flash_erase_partition("nvs"));
}

TEST_CASE("nvs backend: values persist across re-init", "[nvs]")
{
    nvs_fixture_wipe();
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ASSERT_FALSE(espos_config_storage_was_reset());
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 77));
    TEST_ESP_OK(espos_config_set_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, "persist"));
    TEST_ESP_OK(espos_config_set_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, 1.25f));
    TEST_ESP_OK(espos_config_set_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, false));
    const uint8_t data[5] = { 9, 8, 7, 6, 5 };
    TEST_ESP_OK(espos_config_set_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, data, 5));
    espos_config_deinit();
    (void)nvs_flash_deinit_partition("nvs"); /* simulate reboot */

    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    int32_t i;
    char s[16];
    float f;
    bool b;
    uint8_t blob[8];
    size_t blen = sizeof(blob);
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(77, i);
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("persist", s);
    TEST_ESP_OK(espos_config_get_float(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_RATIO, &f));
    TEST_ASSERT_EQUAL_FLOAT(1.25f, f);
    TEST_ESP_OK(espos_config_get_bool(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_FLAG, &b));
    TEST_ASSERT_FALSE(b);
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, blob, &blen));
    TEST_ASSERT_EQUAL(5, blen);
    TEST_ASSERT_EQUAL_MEMORY(data, blob, 5);
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version(ESPOS_CFG_NS_MIG, &stored, NULL));
    TEST_ASSERT_EQUAL(3, stored);
    espos_config_deinit();
}

TEST_CASE("nvs backend: type change of a key is handled (erase + rewrite)", "[nvs]")
{
    nvs_fixture_wipe();
    /* Plant an old-style string under an int key directly with the NVS API */
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
    nvs_handle_t h;
    TEST_ESP_OK(nvs_open_from_partition("nvs", "t1", NVS_READWRITE, &h));
    TEST_ESP_OK(nvs_set_str(h, "count", "old"));
    TEST_ESP_OK(nvs_commit(h));
    nvs_close(h);
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i); /* type mismatch → default */
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 5)); /* must succeed */
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(5, i);
    espos_config_deinit();
}

TEST_CASE("nvs backend: factory reset erases the partition", "[nvs]")
{
    nvs_fixture_wipe();
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 77));
    TEST_ESP_OK(espos_config_factory_reset());
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 78)); /* usable after reset */
    espos_config_deinit();
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(78, i);
    espos_config_deinit();
}

TEST_CASE("nvs backend: garbage pages are tolerated by NVS itself", "[nvs]")
{
    nvs_fixture_wipe();
    /* Random garbage: NVS marks such pages CORRUPT and reclaims them, so init
     * succeeds without our erase path; defaults are served either way. */
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    TEST_ASSERT_NOT_NULL(p);
    uint8_t *junk = malloc(4096);
    for (int i = 0; i < 4096; i++) {
        junk[i] = (uint8_t)(i * 7 + 3);
    }
    for (size_t off = 0; off < p->size; off += 4096) {
        TEST_ESP_OK(esp_partition_write(p, off, junk, 4096));
    }
    free(junk);
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 1));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(1, i);
    espos_config_deinit();
}

TEST_CASE("nvs backend: an unusable partition (newer NVS format) is erased and defaults are served", "[nvs]")
{
    nvs_fixture_wipe();
    /* Craft a valid ACTIVE page header claiming a NEWER NVS format version
     * (versions count down from 0xff; current is 0xfe). nvs_flash_init then
     * fails with ESP_ERR_NVS_NEW_VERSION_FOUND — the same class of failure
     * as a real-world downgrade or a foreign partition — and the backend must
     * erase and start clean. */
    const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
    TEST_ASSERT_NOT_NULL(p);
    uint8_t hdr[32];
    memset(hdr, 0xff, sizeof(hdr));
    const uint32_t state_active = 0xfffffffe; /* UNINITIALIZED & ~PSB_INIT */
    const uint32_t seq = 0;
    memcpy(hdr, &state_active, 4);
    memcpy(hdr + 4, &seq, 4);
    hdr[8] = 0xfd; /* "newer" than 0xfe */
    uint32_t crc = esp_rom_crc32_le(0xffffffff, hdr + 4, 28 - 4);
    memcpy(hdr + 28, &crc, 4);
    TEST_ESP_OK(esp_partition_write(p, 0, hdr, sizeof(hdr)));
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ASSERT_EQUAL(ESP_ERR_NVS_NEW_VERSION_FOUND, nvs_flash_init_partition("nvs")); /* precondition */
    (void)nvs_flash_deinit_partition("nvs");

    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ASSERT_TRUE(espos_config_storage_was_reset());
    int32_t i;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(42, i);
    TEST_ESP_OK(espos_config_set_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, 1));
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(1, i);
    /* the flag is per boot: a clean re-init reports false */
    espos_config_deinit();
    (void)nvs_flash_deinit_partition("nvs");
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ASSERT_FALSE(espos_config_storage_was_reset());
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_COUNT, &i));
    TEST_ASSERT_EQUAL_INT32(1, i);
    espos_config_deinit();
}

static esp_err_t nvs_mig_1_to_2(espos_config_migrate_ctx_t *ctx, void *arg)
{
    (void)arg;
    int32_t seconds = 0;
    size_t len = sizeof(seconds);
    if (espos_config_migrate_get(ctx, "speed", ESPOS_CFG_TYPE_INT, &seconds, &len) == ESP_OK) {
        int32_t ms = seconds * 1000;
        espos_config_migrate_set(ctx, "speed_ms", ESPOS_CFG_TYPE_INT, &ms, sizeof(ms));
        espos_config_migrate_erase(ctx, "speed");
    }
    return ESP_OK;
}

TEST_CASE("nvs backend: migration chain and stored-value fallbacks on real NVS", "[nvs]")
{
    nvs_fixture_wipe();
    TEST_ESP_OK(nvs_flash_init_partition("nvs"));
    nvs_handle_t h;
    TEST_ESP_OK(nvs_open_from_partition("nvs", "mig", NVS_READWRITE, &h));
    TEST_ESP_OK(nvs_set_i32(h, ESPOS_CFG_VERSION_KEY, 1));
    TEST_ESP_OK(nvs_set_i32(h, "speed", 4));
    TEST_ESP_OK(nvs_commit(h));
    nvs_close(h);
    TEST_ESP_OK(nvs_open_from_partition("nvs", "t1", NVS_READWRITE, &h));
    TEST_ESP_OK(nvs_set_str(h, ESPOS_CFG_VERSION_KEY, "one"));      /* wrong-type stamp */
    TEST_ESP_OK(nvs_set_str(h, "name", "far-too-long-for-eight"));   /* over maxLength */
    const uint8_t twelve[12] = { 1 };
    TEST_ESP_OK(nvs_set_blob(h, "blob", twelve, sizeof(twelve)));   /* over maxLength */
    TEST_ESP_OK(nvs_commit(h));
    nvs_close(h);
    TEST_ESP_OK(espos_config_register_migration("mig", 1, nvs_mig_1_to_2, NULL));
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    uint16_t stored = 0;
    TEST_ESP_OK(espos_config_get_version("mig", &stored, NULL));
    TEST_ASSERT_EQUAL(3, stored);
    int32_t ms = 0;
    TEST_ESP_OK(espos_config_get_i32(ESPOS_CFG_NS_MIG, ESPOS_CFG_MIG_SPEED_MS, &ms));
    TEST_ASSERT_EQUAL_INT32(4000, ms);
    TEST_ESP_OK(espos_config_get_version("t1", &stored, NULL));
    TEST_ASSERT_EQUAL(1, stored);
    char s[16];
    TEST_ESP_OK(espos_config_get_str(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_NAME, s, sizeof(s), NULL));
    TEST_ASSERT_EQUAL_STRING("hello", s);
    size_t blen = 0;
    TEST_ESP_OK(espos_config_get_blob(ESPOS_CFG_NS_T1, ESPOS_CFG_T1_BLOB, NULL, &blen));
    TEST_ASSERT_EQUAL(0, blen);
    espos_config_deinit();
}

TEST_CASE("nvs backend: init on a partition full of 0xFF pages", "[nvs]")
{
    nvs_fixture_wipe();
    TEST_ESP_OK(espos_config_init(espos_config_backend_nvs(), NULL));
    TEST_ASSERT_FALSE(espos_config_storage_was_reset());
    espos_config_deinit();
}
