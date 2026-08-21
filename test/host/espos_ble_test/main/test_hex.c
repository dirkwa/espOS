/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Hex encode/decode. The case asymmetry is a contract with signalk-server,
 * not a style choice, so it is pinned here.
 */
#include <string.h>

#include "ble_proto.h"
#include "unity.h"

TEST_CASE("adv_data hex is UPPERCASE", "[hex]")
{
    const uint8_t in[] = {0x02, 0x01, 0x06, 0xab, 0xcd, 0xef};
    char out[13];
    espos_ble_hex_encode_upper(in, sizeof(in), out);
    TEST_ASSERT_EQUAL_STRING("020106ABCDEF", out);
}

TEST_CASE("gatt_data hex is lowercase", "[hex]")
{
    const uint8_t in[] = {0x02, 0x01, 0x06, 0xab, 0xcd, 0xef};
    char out[13];
    espos_ble_hex_encode_lower(in, sizeof(in), out);
    TEST_ASSERT_EQUAL_STRING("020106abcdef", out);
}

TEST_CASE("empty payload encodes to an empty string", "[hex]")
{
    char out[1];
    espos_ble_hex_encode_upper((const uint8_t *)"", 0, out);
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("decode accepts both cases", "[hex]")
{
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(3, espos_ble_hex_decode("aaBB01", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_HEX8(0xaa, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0xbb, buf[1]);
    TEST_ASSERT_EQUAL_HEX8(0x01, buf[2]);
}

TEST_CASE("decode round-trips an encode", "[hex]")
{
    const uint8_t in[] = {0x00, 0xff, 0x10, 0x7f, 0x80};
    char hex[11];
    uint8_t back[5];
    espos_ble_hex_encode_lower(in, sizeof(in), hex);
    TEST_ASSERT_EQUAL_INT(5, espos_ble_hex_decode(hex, back, sizeof(back)));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(in, back, sizeof(in));
}

/* Malformed input must fail loudly. Silently truncating an odd-length string
 * would turn a corrupt server command into a plausible-looking half write. */
TEST_CASE("decode rejects odd length", "[hex]")
{
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-1, espos_ble_hex_decode("abc", buf, sizeof(buf)));
}

TEST_CASE("decode rejects non-hex digits", "[hex]")
{
    uint8_t buf[8];
    TEST_ASSERT_EQUAL_INT(-1, espos_ble_hex_decode("zz", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(-1, espos_ble_hex_decode("0g", buf, sizeof(buf)));
}

TEST_CASE("decode refuses to overflow the output", "[hex]")
{
    uint8_t buf[2];
    TEST_ASSERT_EQUAL_INT(-1, espos_ble_hex_decode("aabbcc", buf, sizeof(buf)));
}
