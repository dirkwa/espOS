/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * candump ASCII codec.
 *
 * This is the format canboatjs and SignalK's n2k-ip-gateway source read off
 * the wire, so a mistake here is not a crash — it is a gateway that streams
 * frames nobody can parse, or worse, frames that parse into the wrong PGN.
 * It could not be tested at all until the codec stopped naming the TWAI
 * driver's frame type, which is half of why that change was worth making.
 */
#include <cstring>
#include <initializer_list>
#include <string>

#include "espos_n2k/candump_format.h"
#include "unity.h"

using espos_n2k::CanMessage;

namespace {

CanMessage frame(uint32_t id, std::initializer_list<uint8_t> bytes)
{
    CanMessage m = {};
    m.frame.id = id;
    m.frame.extended = true;
    m.frame.dlc = (uint8_t)bytes.size();
    size_t i = 0;
    for (uint8_t b : bytes) m.frame.data[i++] = b;
    m.timestamp_us = 1234567890123456LL;
    return m;
}

/* Everything after the timestamp, which is wall-clock and not ours to
 * predict — candump_encode() stamps with gettimeofday() once the clock is
 * set, precisely so a SignalK server sees real times. */
std::string body(const char *line)
{
    const char *close = strchr(line, ')');
    return close ? std::string(close + 2) : std::string(line);
}

}  // namespace

TEST_CASE("an extended frame encodes as 8 hex digits and upper-case data", "[candump]")
{
    char buf[128];
    const CanMessage m = frame(0x09F80203, {0xFF, 0x00, 0xA5, 0x5A});
    const int n = espos_n2k::candump_encode(m, "can0", buf, sizeof(buf));

    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)n, (uint32_t)strlen(buf));
    TEST_ASSERT_EQUAL_STRING("can0 09F80203#FF00A55A\n", body(buf).c_str());
    /* The line is timestamped, in parentheses, first. */
    TEST_ASSERT_EQUAL_CHAR('(', buf[0]);
}

TEST_CASE("a frame with no data still names the id", "[candump]")
{
    char buf[128];
    const CanMessage m = frame(0x18EAFF00, {});
    TEST_ASSERT_GREATER_THAN_INT(0, espos_n2k::candump_encode(m, "can0", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("can0 18EAFF00#\n", body(buf).c_str());
}

/* Eight bytes is the whole of classic CAN; a longer dlc is a corrupt frame,
 * and writing past eight would read past the payload. */
TEST_CASE("data is clamped to eight bytes", "[candump]")
{
    char buf[128];
    CanMessage m = frame(0x0DF00203, {1, 2, 3, 4, 5, 6, 7, 8});
    m.frame.dlc = 15;   /* as a corrupt bus frame might claim */
    TEST_ASSERT_GREATER_THAN_INT(0, espos_n2k::candump_encode(m, "can0", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("can0 0DF00203#0102030405060708\n", body(buf).c_str());
}

TEST_CASE("a buffer too small is refused, not overrun", "[candump]")
{
    char small[8];
    memset(small, 'x', sizeof(small));
    const CanMessage m = frame(0x09F80203, {0xFF, 0x00});
    TEST_ASSERT_EQUAL_INT(-1, espos_n2k::candump_encode(m, "can0", small, sizeof(small)));
}

TEST_CASE("a candump line decodes to the frame it names", "[candump]")
{
    CanMessage got = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode(
        "(1755600000.123456) can0 09F80203#FF00A55A\n", &got));

    TEST_ASSERT_EQUAL_HEX32(0x09F80203, got.frame.id);
    TEST_ASSERT_TRUE(got.frame.extended);
    TEST_ASSERT_FALSE(got.frame.remote);
    TEST_ASSERT_EQUAL_UINT8(4, got.frame.dlc);
    const uint8_t want[4] = {0xFF, 0x00, 0xA5, 0x5A};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got.frame.data, 4);
    TEST_ASSERT_EQUAL_INT64(1755600000123456LL, got.timestamp_us);
}

TEST_CASE("lower-case hex and a missing newline decode the same", "[candump]")
{
    CanMessage a = {}, b = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode("(1.000000) can0 09f80203#ff00a55a", &a));
    TEST_ASSERT_TRUE(espos_n2k::candump_decode("(1.000000) can0 09F80203#FF00A55A\r\n", &b));
    TEST_ASSERT_EQUAL_HEX32(a.frame.id, b.frame.id);
    TEST_ASSERT_EQUAL_UINT8(a.frame.dlc, b.frame.dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(a.frame.data, b.frame.data, 4);
}

/* candump's own output has no timestamp when run without -t; a client may
 * also just paste an id and data. */
TEST_CASE("a line without a timestamp still decodes", "[candump]")
{
    CanMessage got = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode("can0 09F80203#FF00", &got));
    TEST_ASSERT_EQUAL_HEX32(0x09F80203, got.frame.id);
    TEST_ASSERT_EQUAL_UINT8(2, got.frame.dlc);
    TEST_ASSERT_EQUAL_INT64(0, got.timestamp_us);
}

TEST_CASE("more than eight data bytes on the wire are truncated", "[candump]")
{
    CanMessage got = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode(
        "(1.000000) can0 09F80203#0102030405060708FFFF", &got));
    TEST_ASSERT_EQUAL_UINT8(8, got.frame.dlc);
    const uint8_t want[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, got.frame.data, 8);
}

/* Anything a client can send arrives here: the decoder feeds the CAN bus, so
 * a line it should have rejected becomes a frame on somebody's boat. */
TEST_CASE("junk is rejected", "[candump]")
{
    CanMessage got = {};
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("", &got));
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("hello", &got));
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("(1.0) can0 09F80203", &got));   /* no # */
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("(1.0) can0 09F80203#", &got));  /* no data */
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("(1.0) can0 09F80203#Z", &got)); /* not hex */
    TEST_ASSERT_FALSE(espos_n2k::candump_decode(nullptr, &got));
    TEST_ASSERT_FALSE(espos_n2k::candump_decode("(1.0) can0 09F80203#FF", nullptr));
}

/* What the gateway actually does: frames in from the bus, out to a client,
 * back in from a client, onto the bus. */
TEST_CASE("encode and decode round-trip", "[candump]")
{
    char buf[128];
    const CanMessage sent = frame(0x1DEFFF03, {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF});
    TEST_ASSERT_GREATER_THAN_INT(0, espos_n2k::candump_encode(sent, "can0", buf, sizeof(buf)));

    CanMessage got = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode(buf, &got));
    TEST_ASSERT_EQUAL_HEX32(sent.frame.id, got.frame.id);
    TEST_ASSERT_EQUAL_UINT8(sent.frame.dlc, got.frame.dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sent.frame.data, got.frame.data, sent.frame.dlc);
    TEST_ASSERT_TRUE(got.frame.extended);
}
