/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution
 *
 * Wyoming event builders and parsers.
 *
 * These are the satellite's half of a contract owned by someone else
 * (rhasspy/wyoming, and signalk-wyoming on the orchestrator side). The
 * failure mode for getting a field name or a null-vs-absent distinction
 * wrong is an orchestrator that ignores the satellite without saying why,
 * so the assertions here are deliberately literal about the wire shape.
 */
#include <string>
#include <vector>

#include "ArduinoJson.h"
#include "espos_voice/protocol/events.h"
#include "unity.h"

using espos_voice::AudioFormat;
using espos_voice::DecodedEvent;
using espos_voice::EventDecoder;
using espos_voice::SatelliteInfo;

namespace {

struct Decoded {
    std::string type;
    std::string data_json;
    std::vector<uint8_t> payload;
    bool got = false;
};

bool take_one(void *ctx, const DecodedEvent &ev)
{
    Decoded *d = static_cast<Decoded *>(ctx);
    d->type = ev.type;
    d->data_json = ev.data_json;
    if (ev.payload && ev.payload_len) {
        d->payload.assign(ev.payload, ev.payload + ev.payload_len);
    }
    d->got = true;
    return true;
}

/* Round-trip freshly built bytes back through the decoder — the parsers take
 * a DecodedEvent, so this is also how the satellite itself sees them. */
Decoded decode_one(const std::vector<uint8_t> &wire)
{
    EventDecoder dec;
    Decoded d;
    TEST_ASSERT_TRUE(dec.feed(wire.data(), wire.size(), take_one, &d));
    TEST_ASSERT_TRUE(d.got);
    return d;
}

DecodedEvent as_event(const Decoded &d)
{
    DecodedEvent ev;
    ev.type = d.type;
    ev.data_json = d.data_json;
    return ev;
}

}  // namespace

TEST_CASE("info advertises one mic and one snd program with our formats", "[events]")
{
    SatelliteInfo info;
    info.name = "cockpit";
    info.mic_format.rate = 16000;
    info.snd_format.rate = 22050;
    info.snd_format.channels = 1;

    std::vector<uint8_t> wire;
    espos_voice::build_info(wire, info);
    const Decoded d = decode_one(wire);
    TEST_ASSERT_EQUAL_STRING("info", d.type.c_str());

    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, d.data_json));

    /* All seven program lists present, five of them empty. */
    for (const char *empty : {"asr", "tts", "handle", "intent", "wake"}) {
        TEST_ASSERT_TRUE_MESSAGE(doc[empty].is<JsonArray>(), empty);
        TEST_ASSERT_EQUAL_UINT32(0, (uint32_t)doc[empty].as<JsonArray>().size());
    }
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)doc["mic"].as<JsonArray>().size());
    TEST_ASSERT_EQUAL_UINT32(1, (uint32_t)doc["snd"].as<JsonArray>().size());

    TEST_ASSERT_EQUAL_STRING("cockpit", doc["mic"][0]["name"] | "");
    TEST_ASSERT_EQUAL_UINT32(16000, doc["mic"][0]["mic_format"]["rate"] | 0u);
    TEST_ASSERT_EQUAL_UINT32(22050, doc["snd"][0]["snd_format"]["rate"] | 0u);
    TEST_ASSERT_EQUAL_UINT32(2, doc["snd"][0]["snd_format"]["width"] | 0u);

    TEST_ASSERT_EQUAL_STRING("cockpit", doc["satellite"]["name"] | "");
    TEST_ASSERT_TRUE(doc["satellite"]["supports_trigger"] | false);
    TEST_ASSERT_TRUE(doc["satellite"]["active_wake_words"].is<JsonArray>());
}

/* null vs absent: the reference sends `text: null`, and an orchestrator that
 * checks for the key's presence would mis-read an omitted field. */
TEST_CASE("pong sends text null when there was none", "[events]")
{
    std::vector<uint8_t> wire;
    espos_voice::build_pong(wire, "");
    const Decoded d = decode_one(wire);

    TEST_ASSERT_EQUAL_STRING("pong", d.type.c_str());
    TEST_ASSERT_EQUAL_STRING("{\"text\":null}", d.data_json.c_str());
}

TEST_CASE("pong echoes the ping's text", "[events]")
{
    std::vector<uint8_t> wire;
    espos_voice::build_pong(wire, "hello");
    const Decoded d = decode_one(wire);
    TEST_ASSERT_EQUAL_STRING("{\"text\":\"hello\"}", d.data_json.c_str());
}

TEST_CASE("run-pipeline asks for asr..tts and names the satellite", "[events]")
{
    std::vector<uint8_t> wire;
    espos_voice::build_run_pipeline(wire, "cockpit");
    const Decoded d = decode_one(wire);
    TEST_ASSERT_EQUAL_STRING("run-pipeline", d.type.c_str());

    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, d.data_json));
    TEST_ASSERT_EQUAL_STRING("asr", doc["start_stage"] | "");
    TEST_ASSERT_EQUAL_STRING("tts", doc["end_stage"] | "");
    TEST_ASSERT_FALSE(doc["restart_on_end"] | true);
    TEST_ASSERT_EQUAL_STRING("cockpit", doc["name"] | "");
}

TEST_CASE("detect with no names listens for any wake word", "[events]")
{
    std::vector<uint8_t> wire;
    espos_voice::build_detect(wire, {});
    TEST_ASSERT_EQUAL_STRING("{\"names\":null}", decode_one(wire).data_json.c_str());

    std::vector<uint8_t> named;
    espos_voice::build_detect(named, {"hey_cockpit"});
    TEST_ASSERT_EQUAL_STRING("{\"names\":[\"hey_cockpit\"]}",
                             decode_one(named).data_json.c_str());
}

/* 16-bit samples go out as little-endian bytes; a byte-order slip here is
 * white noise at the orchestrator, not an error. */
TEST_CASE("audio-chunk carries the PCM frames verbatim", "[events]")
{
    AudioFormat fmt;
    fmt.rate = 16000;
    const int16_t samples[3] = {0x0102, -2, 0x7fff};

    std::vector<uint8_t> wire;
    espos_voice::build_audio_chunk(wire, fmt, samples, 3);
    const Decoded d = decode_one(wire);

    TEST_ASSERT_EQUAL_STRING("audio-chunk", d.type.c_str());
    TEST_ASSERT_EQUAL_UINT32(sizeof(samples), (uint32_t)d.payload.size());
    const uint8_t want[6] = {0x02, 0x01, 0xfe, 0xff, 0xff, 0x7f};
    TEST_ASSERT_EQUAL_HEX8_ARRAY(want, d.payload.data(), sizeof(want));
}

TEST_CASE("audio-start round-trips through parse_audio_start", "[events]")
{
    AudioFormat sent;
    sent.rate = 22050;
    sent.width = 2;
    sent.channels = 1;

    std::vector<uint8_t> wire;
    espos_voice::build_audio_start(wire, sent);
    const Decoded d = decode_one(wire);

    AudioFormat got;
    TEST_ASSERT_TRUE(espos_voice::parse_audio_start(as_event(d), &got));
    TEST_ASSERT_EQUAL_UINT32(22050, got.rate);
    TEST_ASSERT_EQUAL_UINT8(2, got.width);
    TEST_ASSERT_EQUAL_UINT8(1, got.channels);
}

/* A zero rate would divide by zero downstream when sizing the playback
 * buffer; the parser has to reject it rather than pass it on. */
TEST_CASE("parse_audio_start rejects an unusable format", "[events]")
{
    DecodedEvent ev;
    ev.type = "audio-start";
    ev.data_json = "{\"rate\":0,\"width\":2,\"channels\":1}";
    AudioFormat got;
    TEST_ASSERT_FALSE(espos_voice::parse_audio_start(ev, &got));

    ev.data_json = "{\"width\":2,\"channels\":1}";  // rate absent
    TEST_ASSERT_FALSE(espos_voice::parse_audio_start(ev, &got));

    ev.type = "audio-chunk";  // right shape, wrong event
    ev.data_json = "{\"rate\":16000,\"width\":2,\"channels\":1}";
    TEST_ASSERT_FALSE(espos_voice::parse_audio_start(ev, &got));
}

TEST_CASE("parse_transcript wants a transcript with text", "[events]")
{
    DecodedEvent ev;
    std::string text;

    ev.type = "transcript";
    ev.data_json = "{\"text\":\"steer two seven zero\"}";
    TEST_ASSERT_TRUE(espos_voice::parse_transcript(ev, &text));
    TEST_ASSERT_EQUAL_STRING("steer two seven zero", text.c_str());

    ev.data_json = "{\"text\":null}";
    TEST_ASSERT_FALSE(espos_voice::parse_transcript(ev, &text));

    ev.data_json = "";
    TEST_ASSERT_FALSE(espos_voice::parse_transcript(ev, &text));

    ev.type = "detection";
    ev.data_json = "{\"text\":\"x\"}";
    TEST_ASSERT_FALSE(espos_voice::parse_transcript(ev, &text));
}

/* A detection with no data is still a detection — the wake word fired. */
TEST_CASE("parse_detection accepts a bare detection", "[events]")
{
    DecodedEvent ev;
    std::string name = "stale";

    ev.type = "detection";
    ev.data_json = "";
    TEST_ASSERT_TRUE(espos_voice::parse_detection(ev, &name));
    TEST_ASSERT_EQUAL_STRING("", name.c_str());

    ev.data_json = "{\"name\":\"hey_cockpit\"}";
    TEST_ASSERT_TRUE(espos_voice::parse_detection(ev, &name));
    TEST_ASSERT_EQUAL_STRING("hey_cockpit", name.c_str());

    ev.type = "transcript";
    TEST_ASSERT_FALSE(espos_voice::parse_detection(ev, &name));
}

TEST_CASE("parse_ping_text tolerates a ping with nothing in it", "[events]")
{
    DecodedEvent ev;
    ev.type = "ping";

    ev.data_json = "";
    TEST_ASSERT_EQUAL_STRING("", espos_voice::parse_ping_text(ev).c_str());

    ev.data_json = "{\"text\":null}";
    TEST_ASSERT_EQUAL_STRING("", espos_voice::parse_ping_text(ev).c_str());

    ev.data_json = "not json";
    TEST_ASSERT_EQUAL_STRING("", espos_voice::parse_ping_text(ev).c_str());

    ev.data_json = "{\"text\":\"knock\"}";
    TEST_ASSERT_EQUAL_STRING("knock", espos_voice::parse_ping_text(ev).c_str());
}
