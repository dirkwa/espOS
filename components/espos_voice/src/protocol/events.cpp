/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "espos_voice/protocol/events.h"

#include <memory>

#include "cJSON.h"

namespace espos_voice {

namespace {

struct JsonDeleter {
  void operator()(cJSON* j) const { cJSON_Delete(j); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

struct JsonTextDeleter {
  void operator()(char* p) const { cJSON_free(p); }
};
using JsonText = std::unique_ptr<char, JsonTextDeleter>;

void add_format(cJSON* obj, const char* key, const AudioFormat& fmt) {
  cJSON* f = cJSON_AddObjectToObject(obj, key);
  if (!f) return;
  cJSON_AddNumberToObject(f, "rate", fmt.rate);
  cJSON_AddNumberToObject(f, "width", fmt.width);
  cJSON_AddNumberToObject(f, "channels", fmt.channels);
}

std::string dump(const JsonPtr& doc) {
  if (!doc) return std::string();
  JsonText text(cJSON_PrintUnformatted(doc.get()));
  return text ? std::string(text.get()) : std::string();
}

// Parse a data block. Returns null for empty or malformed input; every caller
// treats that as "the field is not there", which is also what an event with no
// data block means.
JsonPtr parse(const std::string& json) {
  if (json.empty()) return JsonPtr();
  // require_null_terminated: cJSON otherwise stops at the end of the first
  // value and ignores whatever follows, so {"name":"wake"}garbage would parse
  // as a perfectly good object. The data block is exactly data_length bytes
  // off the wire, so trailing non-whitespace means the sender declared a
  // length its own JSON does not fill -- a frame to refuse, not to interpret.
  // Trailing whitespace is still fine; cJSON skips it before the check.
  //
  // size() + 1 so the string's own NUL is inside the buffer: the check reads
  // the byte after the value and fails if it is past the end.
  JsonPtr doc(cJSON_ParseWithLengthOpts(json.data(), json.size() + 1, nullptr, 1));
  if (!cJSON_IsObject(doc.get())) return JsonPtr();
  return doc;
}

// A string field, or nullptr when absent/null/not a string.
const char* str_field(const JsonPtr& doc, const char* key) {
  if (!doc) return nullptr;
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(doc.get(), key);
  return cJSON_IsString(v) ? v->valuestring : nullptr;
}

// A numeric field, or 0 when absent/null/not a number — matching what the
// callers want, since 0 is never a usable rate/width/channel count.
double num_field(const JsonPtr& doc, const char* key) {
  if (!doc) return 0;
  const cJSON* v = cJSON_GetObjectItemCaseSensitive(doc.get(), key);
  return cJSON_IsNumber(v) ? v->valuedouble : 0;
}

}  // namespace

void build_info(std::vector<uint8_t>& out, const SatelliteInfo& info) {
  JsonPtr doc(cJSON_CreateObject());
  if (!doc) return;
  // Seven program lists are always present (parseInfo tolerates absence,
  // but the reference always serializes them).
  cJSON_AddArrayToObject(doc.get(), "asr");
  cJSON_AddArrayToObject(doc.get(), "tts");
  cJSON_AddArrayToObject(doc.get(), "handle");
  cJSON_AddArrayToObject(doc.get(), "intent");
  cJSON_AddArrayToObject(doc.get(), "wake");

  // We can capture: advertise one mic program with our capture format.
  cJSON* mics = cJSON_AddArrayToObject(doc.get(), "mic");
  cJSON* mic = cJSON_CreateObject();
  if (mics && mic) {
    cJSON_AddItemToArray(mics, mic);
    cJSON_AddStringToObject(mic, "name", info.name.c_str());
    add_format(mic, "mic_format", info.mic_format);
  } else {
    cJSON_Delete(mic);
  }

  // We can play: advertise one snd program with our playback format.
  cJSON* snds = cJSON_AddArrayToObject(doc.get(), "snd");
  cJSON* snd = cJSON_CreateObject();
  if (snds && snd) {
    cJSON_AddItemToArray(snds, snd);
    cJSON_AddStringToObject(snd, "name", info.name.c_str());
    add_format(snd, "snd_format", info.snd_format);
  } else {
    cJSON_Delete(snd);
  }

  cJSON* sat = cJSON_AddObjectToObject(doc.get(), "satellite");
  if (sat) {
    cJSON_AddStringToObject(sat, "name", info.name.c_str());
    cJSON_AddArrayToObject(sat, "active_wake_words");  // empty until Phase 3
    cJSON_AddBoolToObject(sat, "supports_trigger", info.supports_trigger);
  }

  encode_event(out, "info", dump(doc), nullptr, 0);
}

void build_pong(std::vector<uint8_t>& out, const std::string& text) {
  // Reference serializes `text` as null when unset.
  JsonPtr doc(cJSON_CreateObject());
  if (!doc) return;
  if (text.empty()) {
    cJSON_AddNullToObject(doc.get(), "text");
  } else {
    cJSON_AddStringToObject(doc.get(), "text", text.c_str());
  }
  encode_event(out, "pong", dump(doc), nullptr, 0);
}

void build_played(std::vector<uint8_t>& out) { encode_event(out, "played"); }

void build_run_pipeline(std::vector<uint8_t>& out, const std::string& name) {
  // asr..tts: transcribe our mic audio and (if a reply is synthesised) play
  // it back. The orchestrator segments/endpoints the utterance itself.
  JsonPtr doc(cJSON_CreateObject());
  if (!doc) return;
  cJSON_AddStringToObject(doc.get(), "start_stage", "asr");
  cJSON_AddStringToObject(doc.get(), "end_stage", "tts");
  cJSON_AddBoolToObject(doc.get(), "restart_on_end", false);
  if (!name.empty()) cJSON_AddStringToObject(doc.get(), "name", name.c_str());
  encode_event(out, "run-pipeline", dump(doc), nullptr, 0);
}

namespace {
std::string audio_format_json(const AudioFormat& fmt, uint32_t timestamp_ms,
                              bool with_ts) {
  JsonPtr doc(cJSON_CreateObject());
  if (!doc) return std::string();
  cJSON_AddNumberToObject(doc.get(), "rate", fmt.rate);
  cJSON_AddNumberToObject(doc.get(), "width", fmt.width);
  cJSON_AddNumberToObject(doc.get(), "channels", fmt.channels);
  if (with_ts) cJSON_AddNumberToObject(doc.get(), "timestamp", timestamp_ms);
  return dump(doc);
}
}  // namespace

void build_audio_start(std::vector<uint8_t>& out, const AudioFormat& fmt) {
  encode_event(out, "audio-start", audio_format_json(fmt, 0, false), nullptr,
               0);
}

void build_audio_chunk(std::vector<uint8_t>& out, const AudioFormat& fmt,
                       const int16_t* samples, size_t frames) {
  const uint8_t* payload = reinterpret_cast<const uint8_t*>(samples);
  encode_event(out, "audio-chunk", audio_format_json(fmt, 0, false), payload,
               frames * sizeof(int16_t));
}

void build_audio_stop(std::vector<uint8_t>& out) {
  encode_event(out, "audio-stop");
}

void build_detect(std::vector<uint8_t>& out,
                  const std::vector<std::string>& names) {
  // `names` is serialized as null when empty (listen for any wake word),
  // mirroring the reference Detect().
  JsonPtr doc(cJSON_CreateObject());
  if (!doc) return;
  if (names.empty()) {
    cJSON_AddNullToObject(doc.get(), "names");
  } else {
    cJSON* arr = cJSON_AddArrayToObject(doc.get(), "names");
    if (arr) {
      for (const std::string& n : names) {
        cJSON* item = cJSON_CreateString(n.c_str());
        if (item) cJSON_AddItemToArray(arr, item);
      }
    }
  }
  encode_event(out, "detect", dump(doc), nullptr, 0);
}

bool parse_transcript(const DecodedEvent& ev, std::string* text) {
  if (ev.type != "transcript") return false;
  JsonPtr doc = parse(ev.data_json);
  const char* t = str_field(doc, "text");
  if (!t) return false;
  *text = t;
  return true;
}

bool parse_detection(const DecodedEvent& ev, std::string* name) {
  if (ev.type != "detection") return false;
  name->clear();
  if (ev.data_json.empty()) return true;  // detection with no data is valid
  // A data block that will not parse is not a detection with an unknown name,
  // it is a frame we do not understand -- and acting on it means waking the
  // panel because something got garbled. An absent or null `name` inside a
  // well-formed object is different, and still means "the wake word fired".
  JsonPtr doc = parse(ev.data_json);
  if (!doc) return false;
  const char* n = str_field(doc, "name");  // null/absent -> leave empty
  if (n) *name = n;
  return true;
}

bool parse_audio_start(const DecodedEvent& ev, AudioFormat* out) {
  if (ev.type != "audio-start") return false;
  JsonPtr doc = parse(ev.data_json);
  if (!doc) return false;
  AudioFormat fmt;
  fmt.rate = (uint32_t)num_field(doc, "rate");
  fmt.width = (uint8_t)num_field(doc, "width");
  fmt.channels = (uint8_t)num_field(doc, "channels");
  if (!fmt.valid()) return false;
  *out = fmt;
  return true;
}

std::string parse_ping_text(const DecodedEvent& ev) {
  JsonPtr doc = parse(ev.data_json);
  const char* t = str_field(doc, "text");
  return t ? std::string(t) : std::string();
}

}  // namespace espos_voice
