/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Advertisement ring buffer. The drop accounting matters: the firmware this
 * replaces computed the drop count from a size delta taken AFTER the erase,
 * which under-reported every time. "We lost data" has to be visible.
 */
#include <string.h>

#include "ble_proto.h"
#include "unity.h"

static espos_ble_adv_t make_adv(uint8_t id)
{
    espos_ble_adv_t a;
    memset(&a, 0, sizeof(a));
    snprintf(a.address, sizeof(a.address), "AA:BB:CC:DD:EE:%02X", id);
    a.rssi = -40 - (int8_t)id;
    a.adv_data_len = 1;
    a.adv_data[0] = id;
    return a;
}

TEST_CASE("empty queue drains nothing", "[advq]")
{
    espos_ble_adv_t storage[4], out[4];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 4);
    TEST_ASSERT_EQUAL_UINT(0, espos_ble_advq_count(&q));
    TEST_ASSERT_EQUAL_UINT(0, espos_ble_advq_drain(&q, out, 4));
}

TEST_CASE("drain returns entries oldest first", "[advq]")
{
    espos_ble_adv_t storage[4], out[4];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 4);
    for (uint8_t i = 0; i < 3; i++) {
        TEST_ASSERT_FALSE(espos_ble_advq_push(&q, &(espos_ble_adv_t){0}));
    }
    espos_ble_advq_init(&q, storage, 4);
    for (uint8_t i = 0; i < 3; i++) {
        espos_ble_adv_t a = make_adv(i);
        espos_ble_advq_push(&q, &a);
    }
    TEST_ASSERT_EQUAL_UINT(3, espos_ble_advq_drain(&q, out, 4));
    TEST_ASSERT_EQUAL_HEX8(0, out[0].adv_data[0]);
    TEST_ASSERT_EQUAL_HEX8(1, out[1].adv_data[0]);
    TEST_ASSERT_EQUAL_HEX8(2, out[2].adv_data[0]);
    TEST_ASSERT_EQUAL_UINT(0, espos_ble_advq_count(&q));
}

TEST_CASE("a full queue drops the oldest and counts it", "[advq]")
{
    espos_ble_adv_t storage[3], out[3];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 3);

    for (uint8_t i = 0; i < 3; i++) {
        espos_ble_adv_t a = make_adv(i);
        TEST_ASSERT_FALSE(espos_ble_advq_push(&q, &a));
    }
    TEST_ASSERT_EQUAL_UINT(0, q.dropped);

    espos_ble_adv_t a = make_adv(3);
    TEST_ASSERT_TRUE(espos_ble_advq_push(&q, &a)); /* evicts id 0 */
    TEST_ASSERT_EQUAL_UINT(1, q.dropped);
    TEST_ASSERT_EQUAL_UINT(3, espos_ble_advq_count(&q));

    TEST_ASSERT_EQUAL_UINT(3, espos_ble_advq_drain(&q, out, 3));
    TEST_ASSERT_EQUAL_HEX8(1, out[0].adv_data[0]);
    TEST_ASSERT_EQUAL_HEX8(3, out[2].adv_data[0]);
}

/* Every push past capacity is exactly one drop - no double counting, no
 * silent losses. */
TEST_CASE("drop count matches overflow exactly", "[advq]")
{
    espos_ble_adv_t storage[4];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 4);
    for (uint8_t i = 0; i < 20; i++) {
        espos_ble_adv_t a = make_adv(i);
        espos_ble_advq_push(&q, &a);
    }
    TEST_ASSERT_EQUAL_UINT(16, q.dropped);
    TEST_ASSERT_EQUAL_UINT(4, espos_ble_advq_count(&q));
}

TEST_CASE("partial drain keeps the remainder in order", "[advq]")
{
    espos_ble_adv_t storage[8], out[8];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 8);
    for (uint8_t i = 0; i < 5; i++) {
        espos_ble_adv_t a = make_adv(i);
        espos_ble_advq_push(&q, &a);
    }
    TEST_ASSERT_EQUAL_UINT(2, espos_ble_advq_drain(&q, out, 2));
    TEST_ASSERT_EQUAL_HEX8(0, out[0].adv_data[0]);
    TEST_ASSERT_EQUAL_HEX8(1, out[1].adv_data[0]);
    TEST_ASSERT_EQUAL_UINT(3, espos_ble_advq_count(&q));

    TEST_ASSERT_EQUAL_UINT(3, espos_ble_advq_drain(&q, out, 8));
    TEST_ASSERT_EQUAL_HEX8(2, out[0].adv_data[0]);
    TEST_ASSERT_EQUAL_HEX8(4, out[2].adv_data[0]);
}

/* Wrapping is where an off-by-one in the tail calculation shows up. */
TEST_CASE("push/drain cycles survive wraparound", "[advq]")
{
    espos_ble_adv_t storage[3], out[3];
    espos_ble_advq_t q;
    espos_ble_advq_init(&q, storage, 3);
    for (uint8_t round = 0; round < 10; round++) {
        for (uint8_t i = 0; i < 2; i++) {
            espos_ble_adv_t a = make_adv((uint8_t)(round * 2 + i));
            espos_ble_advq_push(&q, &a);
        }
        TEST_ASSERT_EQUAL_UINT(2, espos_ble_advq_drain(&q, out, 3));
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(round * 2), out[0].adv_data[0]);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(round * 2 + 1), out[1].adv_data[0]);
    }
    TEST_ASSERT_EQUAL_UINT(0, q.dropped);
}
