/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "test_common.h"
#include "../../../../components/espos_config/src/espos_config_priv.h"

static void roundtrip(const uint8_t *in, size_t n, const char *expect)
{
    char enc[64];
    size_t w = espos_b64_encode(in, n, enc, sizeof(enc));
    TEST_ASSERT_EQUAL_STRING(expect, enc);
    TEST_ASSERT_EQUAL(strlen(expect), w);
    uint8_t dec[64];
    size_t dn = 0;
    TEST_ESP_OK(espos_b64_decode(enc, strlen(enc), dec, sizeof(dec), &dn));
    TEST_ASSERT_EQUAL(n, dn);
    if (n) {
        TEST_ASSERT_EQUAL_MEMORY(in, dec, n);
    }
}

TEST_CASE("base64 encode/decode RFC 4648 vectors", "[b64]")
{
    roundtrip((const uint8_t *)"", 0, "");
    roundtrip((const uint8_t *)"f", 1, "Zg==");
    roundtrip((const uint8_t *)"fo", 2, "Zm8=");
    roundtrip((const uint8_t *)"foo", 3, "Zm9v");
    roundtrip((const uint8_t *)"foob", 4, "Zm9vYg==");
    roundtrip((const uint8_t *)"fooba", 5, "Zm9vYmE=");
    roundtrip((const uint8_t *)"foobar", 6, "Zm9vYmFy");
    const uint8_t bin[] = { 0xfb, 0xff, 0xbf, 0x00 };
    roundtrip(bin, 4, "+/+/AA==");
}

TEST_CASE("base64 decode edge cases", "[b64]")
{
    uint8_t out[8];
    size_t n = 99;
    /* unpadded input accepted */
    TEST_ESP_OK(espos_b64_decode("Zm9vYg", 6, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(4, n);
    TEST_ASSERT_EQUAL_MEMORY("foob", out, 4);
    /* excess trailing padding is tolerated; '=' anywhere else is not */
    TEST_ESP_OK(espos_b64_decode("Zg===", 5, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(1, n);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_b64_decode("Z=g=", 4, out, sizeof(out), &n));
    /* invalid characters */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_b64_decode("Zm9v*g==", 8, out, sizeof(out), &n));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_b64_decode("Zm9v Yg==", 9, out, sizeof(out), &n));
    /* impossible length (1 mod 4) */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, espos_b64_decode("Zm9vY", 5, out, sizeof(out), &n));
    /* output too small */
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_SIZE, espos_b64_decode("Zm9vYmFy", 8, out, 3, &n));
    /* encode buffer too small */
    char enc[4];
    TEST_ASSERT_EQUAL(0, espos_b64_encode((const uint8_t *)"foo", 3, enc, sizeof(enc)));
    TEST_ASSERT_EQUAL_STRING("", enc);
    TEST_ASSERT_EQUAL(5, espos_b64_encoded_len(3));
    TEST_ASSERT_EQUAL(1, espos_b64_encoded_len(0));
    TEST_ASSERT_EQUAL(9, espos_b64_encoded_len(4));
}
