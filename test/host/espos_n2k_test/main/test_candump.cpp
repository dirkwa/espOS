/* SPDX-FileCopyrightText: 2026 Dirk Wahrheit
 * SPDX-License-Identifier: Apache-2.0
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

namespace
{

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
    const CanMessage m = frame(0x09F80203, { 0xFF, 0x00, 0xA5, 0x5A });
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
    CanMessage m = frame(0x0DF00203, { 1, 2, 3, 4, 5, 6, 7, 8 });
    m.frame.dlc = 15;   /* as a corrupt bus frame might claim */
    TEST_ASSERT_GREATER_THAN_INT(0, espos_n2k::candump_encode(m, "can0", buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_STRING("can0 0DF00203#0102030405060708\n", body(buf).c_str());
}

TEST_CASE("a buffer too small is refused, not overrun", "[candump]")
{
    char small[8];
    memset(small, 'x', sizeof(small));
    const CanMessage m = frame(0x09F80203, { 0xFF, 0x00 });
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
    const uint8_t want[4] = { 0xFF, 0x00, 0xA5, 0x5A };
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
    const uint8_t want[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
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
    const CanMessage sent = frame(0x1DEFFF03, { 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF });
    TEST_ASSERT_GREATER_THAN_INT(0, espos_n2k::candump_encode(sent, "can0", buf, sizeof(buf)));

    CanMessage got = {};
    TEST_ASSERT_TRUE(espos_n2k::candump_decode(buf, &got));
    TEST_ASSERT_EQUAL_HEX32(sent.frame.id, got.frame.id);
    TEST_ASSERT_EQUAL_UINT8(sent.frame.dlc, got.frame.dlc);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(sent.frame.data, got.frame.data, sent.frame.dlc);
    TEST_ASSERT_TRUE(got.frame.extended);
}

/* ── partial sends ──────────────────────────────────────────────────────
 *
 * send() on a nearly-full socket takes fewer bytes than it was offered. The
 * server used to treat any non-negative return as "all of it" and reset the
 * buffer, so the unsent tail vanished mid-line and the next frame was
 * appended onto that fragment. A reader then parsed one corrupt line -- for a
 * CAN id, a plausible WRONG PGN, which is worse than a visible gap.
 *
 * candump_resync_offset() is the recovery decision: resume after the next
 * newline, dropping the half-sent line. */

TEST_CASE("resync: a send that stopped mid-line drops the rest of that line", "[candump][resync]")
{
    /* Three lines; the socket took the first and four bytes of the second. */
    const char buf[] = "AAA\nBBBBBB\nCCC\n";
    const size_t sent = 4 + 4; /* "AAA\n" plus "BBBB" */
    const size_t keep = espos_n2k::candump_resync_offset(buf, sizeof(buf) - 1, sent);
    /* Resume at "CCC\n": the remains of the B line are dropped, not sent as
     * if they were the start of a new one. */
    TEST_ASSERT_EQUAL_size_t(11, keep);
    TEST_ASSERT_EQUAL_STRING("CCC\n", buf + keep);
}

TEST_CASE("resync: a send that stopped exactly on a boundary drops nothing", "[candump][resync]")
{
    const char buf[] = "AAA\nBBB\n";
    const size_t keep = espos_n2k::candump_resync_offset(buf, sizeof(buf) - 1, 4);
    /* The cut is already a line boundary, so there is no partial line to
     * discard and the second line is kept whole. */
    TEST_ASSERT_EQUAL_size_t(4, keep);
    TEST_ASSERT_EQUAL_STRING("BBB\n", buf + keep);
}

TEST_CASE("resync: nothing sent still drops only up to the first boundary", "[candump][resync]")
{
    const char buf[] = "AAA\nBBB\n";
    /* sent == 0 happens on a socket that accepted nothing. The first line is
     * intact, so nothing should be discarded. */
    TEST_ASSERT_EQUAL_size_t(0, espos_n2k::candump_resync_offset(buf, sizeof(buf) - 1, 0));
}

TEST_CASE("resync: a tail with no newline left is dropped whole", "[candump][resync]")
{
    /* The buffer is flushed on a size or time trigger and can end mid-line.
     * With no newline after the cut there is no boundary to resume from, so
     * the remainder goes rather than being kept to be prefixed onto the next
     * frame. */
    const char buf[] = "AAA\nBBBB";
    TEST_ASSERT_EQUAL_size_t(8, espos_n2k::candump_resync_offset(buf, sizeof(buf) - 1, 6));
}

TEST_CASE("resync: a complete send keeps nothing back", "[candump][resync]")
{
    const char buf[] = "AAA\n";
    /* sent == len is the ordinary case; the caller clears the buffer. */
    TEST_ASSERT_EQUAL_size_t(4, espos_n2k::candump_resync_offset(buf, 4, 4));
    /* Defensive: a sent count beyond the buffer must not walk off it. */
    TEST_ASSERT_EQUAL_size_t(4, espos_n2k::candump_resync_offset(buf, 4, 99));
}

TEST_CASE("resync: a real candump line survives being cut anywhere", "[candump][resync]")
{
    /* The property that matters: whatever is kept always starts a line, so a
     * decoder never sees a fragment. Cut a two-line buffer at every offset. */
    char buf[256];
    const CanMessage a = frame(0x09F80203, { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08 });
    const CanMessage b = frame(0x1DEFFF03, { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 });
    int n = espos_n2k::candump_encode(a, "can0", buf, sizeof(buf));
    n += espos_n2k::candump_encode(b, "can0", buf + n, sizeof(buf) - n);

    for (size_t cut = 0; cut <= (size_t)n; cut++) {
        const size_t keep = espos_n2k::candump_resync_offset(buf, (size_t)n, cut);
        TEST_ASSERT_TRUE(keep >= cut);          /* never rewinds */
        TEST_ASSERT_TRUE(keep <= (size_t)n);    /* never past the end */
        if (keep < (size_t)n) {
            /* Whatever is left decodes: it is a whole line, not a tail. */
            CanMessage got = {};
            TEST_ASSERT_TRUE(espos_n2k::candump_decode(buf + keep, &got));
        }
    }
}
