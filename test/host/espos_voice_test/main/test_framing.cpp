/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Wyoming wire framing.
 *
 * The satellite reads this off a TCP socket, so events arrive split at
 * arbitrary byte boundaries and several can share one read. A framing bug
 * therefore shows up on hardware as an occasional silent satellite under
 * load, which is close to undiagnosable from the outside — the split-feed
 * and coalesced-feed cases below are the point of this file.
 */
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "espos_voice/protocol/framing.h"
#include "unity.h"

using espos_voice::DecodedEvent;
using espos_voice::EventDecoder;

namespace {

/* Collect decoded events. The payload view dies with the callback, so copy. */
struct Collected {
    std::string type;
    std::string data_json;
    std::vector<uint8_t> payload;
};

struct Sink {
    std::vector<Collected> events;
    bool keep_going = true;
};

bool collect(void *ctx, const DecodedEvent &ev)
{
    Sink *sink = static_cast<Sink *>(ctx);
    Collected c;
    c.type = ev.type;
    c.data_json = ev.data_json;
    if (ev.payload && ev.payload_len) {
        c.payload.assign(ev.payload, ev.payload + ev.payload_len);
    }
    sink->events.push_back(c);
    return sink->keep_going;
}

std::string as_text(const std::vector<uint8_t> &bytes)
{
    return std::string(reinterpret_cast<const char *>(bytes.data()), bytes.size());
}

std::vector<uint8_t> as_bytes(const std::string &s)
{
    return std::vector<uint8_t>(s.begin(), s.end());
}

/* The header line, i.e. everything up to the first newline. */
std::string header_line(const std::vector<uint8_t> &wire)
{
    const std::string text = as_text(wire);
    return text.substr(0, text.find('\n'));
}

}  // namespace

TEST_CASE("header-only event is one JSON line and nothing else", "[framing]")
{
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "played");

    TEST_ASSERT_EQUAL_STRING("{\"type\":\"played\",\"version\":\"1.10.0\"}",
                             header_line(wire).c_str());
    /* No data block, no payload: the line and its newline are the whole event. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(header_line(wire).size() + 1), (uint32_t)wire.size());
}

/* data_length is a BYTE count and the block follows the newline verbatim,
 * with no separator of its own. */
TEST_CASE("data block is byte-counted and spliced after the header", "[framing]")
{
    const std::string data = "{\"text\":\"heiß\"}";  // multi-byte UTF-8 on purpose
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "transcript", data, nullptr, 0);

    JsonDocument header;
    TEST_ASSERT_FALSE(deserializeJson(header, header_line(wire)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)data.size(), header["data_length"] | 0u);
    TEST_ASSERT_FALSE(header["payload_length"].is<uint32_t>());

    const std::string tail = as_text(wire).substr(header_line(wire).size() + 1);
    TEST_ASSERT_EQUAL_STRING(data.c_str(), tail.c_str());
}

TEST_CASE("empty data is omitted rather than sent as {}", "[framing]")
{
    std::vector<uint8_t> a, b;
    espos_voice::encode_event(a, "played", "", nullptr, 0);
    espos_voice::encode_event(b, "played", "{}", nullptr, 0);

    TEST_ASSERT_FALSE(as_text(a).find("data_length") != std::string::npos);
    TEST_ASSERT_EQUAL_STRING(as_text(a).c_str(), as_text(b).c_str());
}

TEST_CASE("payload follows the data block and is byte-counted", "[framing]")
{
    const uint8_t pcm[6] = {0x01, 0x02, 0x03, 0xfe, 0xff, 0x00};
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "audio-chunk", "{\"rate\":16000}", pcm, sizeof(pcm));

    JsonDocument header;
    TEST_ASSERT_FALSE(deserializeJson(header, header_line(wire)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(pcm), header["payload_length"] | 0u);

    EventDecoder dec;
    Sink sink;
    TEST_ASSERT_TRUE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sink.events.size());
    TEST_ASSERT_EQUAL_STRING("audio-chunk", sink.events[0].type.c_str());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(pcm), (uint32_t)sink.events[0].payload.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pcm, sink.events[0].payload.data(), sizeof(pcm));
}

/* The property that matters on a socket: the decoder must not care where the
 * chunk boundaries fall. */
TEST_CASE("event survives being fed one byte at a time", "[framing]")
{
    const uint8_t pcm[4] = {0xde, 0xad, 0xbe, 0xef};
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "audio-chunk", "{\"rate\":16000}", pcm, sizeof(pcm));

    EventDecoder dec;
    Sink sink;
    for (size_t i = 0; i < wire.size(); ++i) {
        TEST_ASSERT_TRUE(dec.feed(&wire[i], 1, collect, &sink));
        /* Nothing may be emitted before the very last byte arrives. */
        if (i + 1 < wire.size()) {
            TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)sink.events.size());
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sink.events.size());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(pcm), (uint32_t)sink.events[0].payload.size());
    TEST_ASSERT_EQUAL_HEX8_ARRAY(pcm, sink.events[0].payload.data(), sizeof(pcm));
}

TEST_CASE("several events in one chunk are delivered in order", "[framing]")
{
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "audio-start", "{\"rate\":16000}", nullptr, 0);
    const uint8_t pcm[2] = {0x11, 0x22};
    espos_voice::encode_event(wire, "audio-chunk", "{\"rate\":16000}", pcm, sizeof(pcm));
    espos_voice::encode_event(wire, "audio-stop");

    EventDecoder dec;
    Sink sink;
    TEST_ASSERT_TRUE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(3, (uint32_t)sink.events.size());
    TEST_ASSERT_EQUAL_STRING("audio-start", sink.events[0].type.c_str());
    TEST_ASSERT_EQUAL_STRING("audio-chunk", sink.events[1].type.c_str());
    TEST_ASSERT_EQUAL_STRING("audio-stop", sink.events[2].type.c_str());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(pcm), (uint32_t)sink.events[1].payload.size());
}

/* A decoder that kept going after a framing violation would resynchronise on
 * whatever byte happened to look like a newline — the connection has to die. */
TEST_CASE("malformed header JSON fails the decoder for good", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::vector<uint8_t> junk = as_bytes("this is not json\n");
    TEST_ASSERT_FALSE(dec.feed(junk.data(), junk.size(), collect, &sink));
    TEST_ASSERT_TRUE(dec.failed());
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)sink.events.size());

    /* Still failed on the next feed, even with a perfectly good event. */
    std::vector<uint8_t> good;
    espos_voice::encode_event(good, "played");
    TEST_ASSERT_FALSE(dec.feed(good.data(), good.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)sink.events.size());
}

TEST_CASE("header without a type is a framing error", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::vector<uint8_t> wire = as_bytes("{\"version\":\"1.10.0\"}\n");
    TEST_ASSERT_FALSE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_TRUE(dec.failed());
}

TEST_CASE("an oversized data_length is rejected, not allocated", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::string line = "{\"type\":\"x\",\"data_length\":" +
                             std::to_string(EventDecoder::kMaxDataBytes + 1) + "}\n";
    const std::vector<uint8_t> wire = as_bytes(line);
    TEST_ASSERT_FALSE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_TRUE(dec.failed());
}

TEST_CASE("a header line that never ends is rejected at the cap", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::vector<uint8_t> flood(EventDecoder::kMaxHeaderBytes + 2, 'x');
    TEST_ASSERT_FALSE(dec.feed(flood.data(), flood.size(), collect, &sink));
    TEST_ASSERT_TRUE(dec.failed());
}

/* The reference never writes inline data, but tolerating it on read is
 * cheap and some orchestrator versions do. */
TEST_CASE("inline header data is accepted when no block follows", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::vector<uint8_t> wire =
        as_bytes("{\"type\":\"ping\",\"data\":{\"text\":\"hi\"}}\n");
    TEST_ASSERT_TRUE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sink.events.size());
    TEST_ASSERT_EQUAL_STRING("ping", sink.events[0].type.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"text\":\"hi\"}", sink.events[0].data_json.c_str());
}

TEST_CASE("an out-of-line data block wins over inline data", "[framing]")
{
    EventDecoder dec;
    Sink sink;
    const std::vector<uint8_t> wire = as_bytes(
        "{\"type\":\"ping\",\"data\":{\"text\":\"inline\"},\"data_length\":16}\n"
        "{\"text\":\"block\"}");
    TEST_ASSERT_TRUE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sink.events.size());
    TEST_ASSERT_TRUE(sink.events[0].data_json.find("block") != std::string::npos);
}

/* A handler returning false means "drop this connection"; the bytes after it
 * must not be decoded anyway. */
TEST_CASE("a handler that returns false stops the drain", "[framing]")
{
    std::vector<uint8_t> wire;
    espos_voice::encode_event(wire, "played");
    espos_voice::encode_event(wire, "played");

    EventDecoder dec;
    Sink sink;
    sink.keep_going = false;
    TEST_ASSERT_FALSE(dec.feed(wire.data(), wire.size(), collect, &sink));
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)sink.events.size());
}
