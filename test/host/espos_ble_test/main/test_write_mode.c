/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * `with_response` parsing.
 *
 * signalk-server has always sent this field (ble-schemas.ts, on init[],
 * periodic_write[] and gatt_write); the firmware simply never read it and
 * forced write-with-response on every write. Some peripherals - JK-BMS and
 * Daly-BMS among them - reject that with "Write not permitted" and accept
 * only write-without-response.
 *
 * The default is the delicate part: absent MUST mean with-response, or every
 * existing server command that omits the field silently changes behaviour.
 */
#include "ble_proto.h"
#include "cJSON.h"
#include "unity.h"

static espos_ble_write_mode_t mode_of(const char *json)
{
    cJSON *obj = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(obj);
    espos_ble_write_mode_t m = espos_ble_parse_write_mode(obj);
    cJSON_Delete(obj);
    return m;
}

TEST_CASE("absent with_response means WITH response", "[write_mode]")
{
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"uuid\":\"ffe1\",\"data\":\"01\"}"));
}

TEST_CASE("with_response true means WITH response", "[write_mode]")
{
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"uuid\":\"ffe1\",\"with_response\":true}"));
}

/* The case that makes JK/Daly BMSs work at all. */
TEST_CASE("with_response false means WITHOUT response", "[write_mode]")
{
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_NO_RESPONSE,
                          mode_of("{\"uuid\":\"ffe1\",\"with_response\":false}"));
}

TEST_CASE("an empty object defaults to WITH response", "[write_mode]")
{
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE, mode_of("{}"));
}

/* A non-boolean is malformed. Falling back to the safe default beats
 * interpreting 0/"false"/null as a mode nobody asked for. */
TEST_CASE("non-boolean with_response falls back to WITH response", "[write_mode]")
{
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"with_response\":\"false\"}"));
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"with_response\":0}"));
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"with_response\":null}"));
}

TEST_CASE("the field is case-sensitive", "[write_mode]")
{
    /* cJSON's case-sensitive lookup is deliberate: "With_Response" is not the
     * documented field and must not be honoured. */
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          mode_of("{\"With_Response\":false}"));
}

/* Each entry carries its own flag: one no-response write in a list must not
 * change the mode of its neighbours. */
TEST_CASE("per-entry modes are independent", "[write_mode]")
{
    const char *json =
        "{\"init\":[{\"uuid\":\"a\",\"with_response\":false},"
        "{\"uuid\":\"b\"},"
        "{\"uuid\":\"c\",\"with_response\":true}]}";
    cJSON *root = cJSON_Parse(json);
    TEST_ASSERT_NOT_NULL(root);
    cJSON *init = cJSON_GetObjectItemCaseSensitive(root, "init");
    TEST_ASSERT_TRUE(cJSON_IsArray(init));
    TEST_ASSERT_EQUAL_INT(3, cJSON_GetArraySize(init));

    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_NO_RESPONSE,
                          espos_ble_parse_write_mode(cJSON_GetArrayItem(init, 0)));
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          espos_ble_parse_write_mode(cJSON_GetArrayItem(init, 1)));
    TEST_ASSERT_EQUAL_INT(ESPOS_BLE_WRITE_WITH_RESPONSE,
                          espos_ble_parse_write_mode(cJSON_GetArrayItem(init, 2)));
    cJSON_Delete(root);
}
