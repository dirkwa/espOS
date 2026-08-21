/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * UUID text <-> bytes.
 *
 * BLE stores 128-bit UUIDs little-endian in the wire struct while the text
 * form is big-endian, so a reversal here surfaces on hardware as
 * "characteristic not found" - an expensive way to learn about a byte order.
 */
#include <string.h>

#include "ble_proto.h"
#include "unity.h"

TEST_CASE("16-bit UUID expands into the Bluetooth base range", "[uuid]")
{
    uint8_t u[16];
    uint8_t bits = 0;
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("180f", u, &bits));
    TEST_ASSERT_EQUAL_UINT8(16, bits);

    const uint8_t want[16] = {0x00, 0x00, 0x18, 0x0f, 0x00, 0x00, 0x10, 0x00,
                              0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, u, 16);
}

TEST_CASE("32-bit UUID expands into the base range", "[uuid]")
{
    uint8_t u[16];
    uint8_t bits = 0;
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("0000180f", u, &bits));
    TEST_ASSERT_EQUAL_UINT8(32, bits);
    TEST_ASSERT_EQUAL_HEX8(0x18, u[2]);
    TEST_ASSERT_EQUAL_HEX8(0x0f, u[3]);
}

/* Big-endian text order must survive verbatim into the byte array. */
TEST_CASE("128-bit UUID keeps big-endian byte order", "[uuid]")
{
    uint8_t u[16];
    uint8_t bits = 0;
    TEST_ASSERT_TRUE(
        espos_ble_uuid_parse("0000ffe1-0000-1000-8000-00805f9b34fb", u, &bits));
    TEST_ASSERT_EQUAL_UINT8(128, bits);
    TEST_ASSERT_EQUAL_HEX8(0x00, u[0]);
    TEST_ASSERT_EQUAL_HEX8(0xff, u[2]);
    TEST_ASSERT_EQUAL_HEX8(0xe1, u[3]);
    TEST_ASSERT_EQUAL_HEX8(0xfb, u[15]);
}

TEST_CASE("a vendor UUID round-trips unchanged", "[uuid]")
{
    /* JK-BMS-style vendor UUID: outside the base range, so it must stay in
     * the long form rather than being shortened. */
    const char *in = "0000ffe0-0000-1000-8000-00805f9b34fb";
    uint8_t u[16];
    uint8_t bits = 0;
    char out[ESPOS_BLE_UUID_MAX];
    TEST_ASSERT_TRUE(espos_ble_uuid_parse(in, u, &bits));
    espos_ble_uuid_format(u, bits, out);
    TEST_ASSERT_EQUAL_STRING(in, out);
}

TEST_CASE("a non-base 128-bit UUID is never shortened", "[uuid]")
{
    const char *in = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"; /* Nordic UART */
    uint8_t u[16];
    uint8_t bits = 0;
    char out[ESPOS_BLE_UUID_MAX];
    TEST_ASSERT_TRUE(espos_ble_uuid_parse(in, u, &bits));
    espos_ble_uuid_format(u, bits, out);
    TEST_ASSERT_EQUAL_STRING(in, out);
}

TEST_CASE("short forms round-trip as short forms", "[uuid]")
{
    uint8_t u[16];
    uint8_t bits = 0;
    char out[ESPOS_BLE_UUID_MAX];
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("180f", u, &bits));
    espos_ble_uuid_format(u, bits, out);
    TEST_ASSERT_EQUAL_STRING("180f", out);
}

TEST_CASE("formatting is lowercase", "[uuid]")
{
    uint8_t u[16];
    uint8_t bits = 0;
    char out[ESPOS_BLE_UUID_MAX];
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("FFE1", u, &bits));
    espos_ble_uuid_format(u, bits, out);
    TEST_ASSERT_EQUAL_STRING("ffe1", out);
}

/* The whole point of expanding short forms: "180f" and its long spelling are
 * the same attribute and must compare equal. */
TEST_CASE("short and long spellings compare equal", "[uuid]")
{
    uint8_t a[16], b[16];
    uint8_t bits_a = 0, bits_b = 0;
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("180f", a, &bits_a));
    TEST_ASSERT_TRUE(
        espos_ble_uuid_parse("0000180f-0000-1000-8000-00805f9b34fb", b, &bits_b));
    TEST_ASSERT_TRUE(espos_ble_uuid_equal(a, b));
}

TEST_CASE("different UUIDs do not compare equal", "[uuid]")
{
    uint8_t a[16], b[16];
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("180f", a, NULL));
    TEST_ASSERT_TRUE(espos_ble_uuid_parse("180a", b, NULL));
    TEST_ASSERT_FALSE(espos_ble_uuid_equal(a, b));
}

TEST_CASE("malformed UUIDs are rejected", "[uuid]")
{
    uint8_t u[16];
    TEST_ASSERT_FALSE(espos_ble_uuid_parse("18", u, NULL));        /* too short */
    TEST_ASSERT_FALSE(espos_ble_uuid_parse("180fff", u, NULL));    /* odd width */
    TEST_ASSERT_FALSE(espos_ble_uuid_parse("zzzz", u, NULL));      /* not hex */
    TEST_ASSERT_FALSE(espos_ble_uuid_parse("", u, NULL));
    TEST_ASSERT_FALSE(espos_ble_uuid_parse(NULL, u, NULL));
    /* Right length, wrong shape: dashes in the wrong places. */
    TEST_ASSERT_FALSE(
        espos_ble_uuid_parse("0000ffe1-0000-1000-8000-00805f9b34f-", u, NULL));
}
