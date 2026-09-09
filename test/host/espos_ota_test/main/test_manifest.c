/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit */
/* SPDX-License-Identifier: Apache-2.0 */
#include <string.h>

#include "unity.h"
#include "espos_ota_manifest.h"

TEST_CASE("version compare: numeric, prerelease, garbage", "[ota]")
{
    TEST_ASSERT_TRUE(espos_ota_version_cmp("1.2.10", "1.2.9") > 0);
    TEST_ASSERT_TRUE(espos_ota_version_cmp("v1.2.9", "1.2.10") < 0);
    TEST_ASSERT_EQUAL(0, espos_ota_version_cmp("1.2", "1.2.0"));
    TEST_ASSERT_TRUE(espos_ota_version_cmp("2.0.0", "1.99.99") > 0);
    TEST_ASSERT_TRUE(espos_ota_version_cmp("1.0.0-beta.1", "1.0.0") < 0);
    TEST_ASSERT_TRUE(espos_ota_version_cmp("1.0.0-beta.2", "1.0.0-beta.1") > 0);
    TEST_ASSERT_TRUE(espos_ota_version_cmp("1.0.0-rc.1", "0.9.9") > 0);
    TEST_ASSERT_EQUAL(0, espos_ota_version_cmp("1.0.0+build7", "1.0.0"));
    /* git-describe style strings fall back to strcmp: still deterministic */
    TEST_ASSERT_TRUE(espos_ota_version_cmp("abc123-dirty", "0.6.0") != 0);
    TEST_ASSERT_TRUE(espos_ota_version_cmp("0.6.1", "1a09faa-dirty") != 0);
}

TEST_CASE("relative manifest urls resolve against the manifest location", "[ota]")
{
    char out[256];
    TEST_ASSERT_TRUE(espos_ota_resolve_url("https://h/x/manifest.json", "espos.bin", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/x/espos.bin", out);
    TEST_ASSERT_TRUE(espos_ota_resolve_url("https://h/x/manifest.json", "/fw/espos.bin", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/fw/espos.bin", out);
    TEST_ASSERT_TRUE(espos_ota_resolve_url("https://h/x/manifest.json", "http://o/e.bin", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("http://o/e.bin", out);
    TEST_ASSERT_TRUE(espos_ota_resolve_url("https://h", "e.bin", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("https://h/e.bin", out);
    TEST_ASSERT_FALSE(espos_ota_resolve_url("nonsense", "e.bin", out, sizeof(out)));
    TEST_ASSERT_FALSE(espos_ota_resolve_url("https://h/", "", out, sizeof(out)));
}

static const char *MANIFEST =
    "{\"schema\":1,\"app\":\"espos\",\"builds\":["
    "{\"version\":\"0.6.1\",\"target\":\"esp32p4\",\"url\":\"espos-p4-0.6.1.bin\",\"size\":1000,\"sha256\":\"ab\",\"notes\":\"fix\"},"
    "{\"version\":\"0.7.0\",\"target\":\"esp32p4\",\"channel\":\"stable\",\"url\":\"/fw/espos-p4-0.7.0.bin\"},"
    "{\"version\":\"0.8.0-beta.1\",\"target\":\"esp32p4\",\"channel\":\"beta\",\"url\":\"https://x/b.bin\"},"
    "{\"version\":\"9.9.9\",\"target\":\"esp32c6\",\"url\":\"c6.bin\"},"
    "{\"version\":\"9.9.9\",\"target\":\"esp32p4\",\"app\":\"otherapp\",\"url\":\"o.bin\"},"
    "{\"broken\":true}"
    "]}";

TEST_CASE("manifest: highest matching build per target/channel, relative url, newer flag", "[ota]")
{
    espos_ota_build_t b;
    TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/fw/manifest.json",
                                                      "espos", "esp32p4", "stable", "0.6.0", &b));
    TEST_ASSERT_EQUAL_STRING("0.7.0", b.version);
    TEST_ASSERT_EQUAL_STRING("https://h/fw/espos-p4-0.7.0.bin", b.url);
    TEST_ASSERT_TRUE(b.newer);
    TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/fw/manifest.json",
                                                      "espos", "esp32p4", "beta", "0.7.0", &b));
    TEST_ASSERT_EQUAL_STRING("0.8.0-beta.1", b.version);
    TEST_ASSERT_EQUAL_STRING("https://x/b.bin", b.url);
    TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/fw/manifest.json",
                                                      "espos", "esp32p4", "stable", "0.7.0", &b));
    TEST_ASSERT_FALSE(b.newer);       /* same version: not newer */
    TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/fw/manifest.json",
                                                      "espos", "esp32c6", "stable", "1.0.0", &b));
    TEST_ASSERT_EQUAL_STRING("9.9.9", b.version);
    TEST_ASSERT_EQUAL_STRING("https://h/fw/c6.bin", b.url);
}

TEST_CASE("manifest: no match, wrong app, malformed", "[ota]")
{
    espos_ota_build_t b;
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/m.json",
                                                                 "espos", "esp32s3", "stable", "0.1", &b));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, espos_ota_manifest_pick(MANIFEST, strlen(MANIFEST), "https://h/m.json",
                                                                 "someotherapp", "esp32p4", "stable", "0.1", &b));
    const char *bad = "{\"schema\":2,\"builds\":[]}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_ota_manifest_pick(bad, strlen(bad), "https://h/m.json", "espos", "esp32p4", "stable", "0.1", &b));
    const char *nob = "{\"schema\":1}";
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_ota_manifest_pick(nob, strlen(nob), "https://h/m.json", "espos", "esp32p4", "stable", "0.1", &b));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_ota_manifest_pick("[1,2", 4, "https://h/m.json", "espos", "esp32p4", "stable", "0.1", &b));
}

/* From the fuzz harness (UBSan, test/fuzz). "size" is a double out of cJSON
 * and a manifest is free to say 1e999, which parses as infinity; casting
 * that to size_t is undefined behaviour, not a large number. The field
 * decides how big a firmware download this device expects, and the manifest
 * is fetched over the network. */
TEST_CASE("manifest: an unusable size reads as unknown, not as undefined behaviour", "[ota][fuzz]")
{
    espos_ota_build_t b;
    char json[256];

    const char *sizes[] = { "1e999", "-1e999", "1e300", "-5", "0" };
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        snprintf(json, sizeof(json),
                 "{\"schema\":1,\"builds\":[{\"app\":\"espos\",\"target\":\"esp32p4\","
                 "\"version\":\"9.9.9\",\"url\":\"a.bin\",\"size\":%s}]}",
                 sizes[i]);
        TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(json, strlen(json), "https://h/m.json",
                                                          "espos", "esp32p4", "stable", "0.1", &b));
        /* 0 is what the header already defines as "unknown" -- the same
         * answer as a manifest that omitted the field. */
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)b.size);
    }

    /* A size that fits is still carried through. */
    snprintf(json, sizeof(json),
             "{\"schema\":1,\"builds\":[{\"app\":\"espos\",\"target\":\"esp32p4\","
             "\"version\":\"9.9.9\",\"url\":\"a.bin\",\"size\":1048576}]}");
    TEST_ASSERT_EQUAL(ESP_OK, espos_ota_manifest_pick(json, strlen(json), "https://h/m.json",
                                                      "espos", "esp32p4", "stable", "0.1", &b));
    TEST_ASSERT_EQUAL_UINT32(1048576, (uint32_t)b.size);
}
