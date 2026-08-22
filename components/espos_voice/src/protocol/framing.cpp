/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "espos_voice/protocol/framing.h"

#include <cstring>
#include <memory>

#include "cJSON.h"
#include "esp_log.h"

namespace espos_voice {

namespace {

constexpr const char* kTag = "wyoming_frame";

// cJSON hands back raw pointers; these keep the ownership visible at the call
// site instead of relying on every early return remembering to free.
struct JsonDeleter {
  void operator()(cJSON* j) const { cJSON_Delete(j); }
};
using JsonPtr = std::unique_ptr<cJSON, JsonDeleter>;

struct JsonTextDeleter {
  void operator()(char* p) const { cJSON_free(p); }
};
using JsonText = std::unique_ptr<char, JsonTextDeleter>;

}  // namespace

void encode_event(std::vector<uint8_t>& out, const char* type,
                  const std::string& data_json, const uint8_t* payload,
                  size_t payload_len) {
  // The header is itself JSON. Build it with cJSON so escaping is correct,
  // then splice the data block / payload after it. Field order matters only
  // for readability -- cJSON keeps insertion order -- but the byte counts do
  // not include the newline, and the data block has no separator of its own.
  const bool has_data = !data_json.empty() && data_json != "{}";

  JsonPtr header(cJSON_CreateObject());
  if (!header) return;
  cJSON_AddStringToObject(header.get(), "type", type);
  cJSON_AddStringToObject(header.get(), "version", kWyomingVersion);
  if (has_data) {
    cJSON_AddNumberToObject(header.get(), "data_length", (double)data_json.size());
  }
  if (payload && payload_len > 0) {
    cJSON_AddNumberToObject(header.get(), "payload_length", (double)payload_len);
  }

  JsonText header_line(cJSON_PrintUnformatted(header.get()));
  if (!header_line) return;
  const size_t line_len = strlen(header_line.get());

  out.insert(out.end(), header_line.get(), header_line.get() + line_len);
  out.push_back('\n');
  if (has_data) out.insert(out.end(), data_json.begin(), data_json.end());
  if (payload && payload_len > 0) {
    out.insert(out.end(), payload, payload + payload_len);
  }
}

void encode_event(std::vector<uint8_t>& out, const char* type) {
  encode_event(out, type, std::string(), nullptr, 0);
}

void EventDecoder::compact() {
  if (pos_ > 0) {
    buf_.erase(buf_.begin(), buf_.begin() + pos_);
    pos_ = 0;
  }
}

bool EventDecoder::read_header() {
  // Find the newline that terminates the header line.
  size_t nl = std::string::npos;
  for (size_t i = pos_; i < buf_.size(); ++i) {
    if (buf_[i] == '\n') {
      nl = i;
      break;
    }
  }
  if (nl == std::string::npos) {
    if (buf_.size() - pos_ > kMaxHeaderBytes) {
      ESP_LOGE(kTag, "header line exceeds %u bytes", (unsigned)kMaxHeaderBytes);
      failed_ = true;
    }
    return false;  // need more bytes
  }

  std::string line((const char*)&buf_[pos_], nl - pos_);
  pos_ = nl + 1;

  JsonPtr doc(cJSON_ParseWithLength(line.data(), line.size()));
  if (!doc || !cJSON_IsObject(doc.get())) {
    ESP_LOGE(kTag, "malformed header JSON");
    failed_ = true;
    return false;
  }

  const cJSON* type = cJSON_GetObjectItemCaseSensitive(doc.get(), "type");
  if (!cJSON_IsString(type) || !type->valuestring || type->valuestring[0] == '\0') {
    ESP_LOGE(kTag, "header missing \"type\"");
    failed_ = true;
    return false;
  }
  type_ = type->valuestring;

  // Lengths: absent/null/non-numeric => 0. Reject negatives, non-integers and
  // anything past the cap -- these sizes are what the decoder is about to
  // trust when it waits for that many bytes.
  double data_len = 0, payload_len = 0;
  const cJSON* dl = cJSON_GetObjectItemCaseSensitive(doc.get(), "data_length");
  const cJSON* pl = cJSON_GetObjectItemCaseSensitive(doc.get(), "payload_length");
  if (cJSON_IsNumber(dl)) data_len = dl->valuedouble;
  if (cJSON_IsNumber(pl)) payload_len = pl->valuedouble;
  // Both bounds are explicit. The pre-cJSON code read these through a signed
  // long and so could never see a payload_length past LONG_MAX, which on a
  // 32-bit target kept data_len_ + payload_len_ from wrapping -- an accident
  // of the type, not a check. Reading them as JSON numbers removes it, so the
  // ceiling has to be stated. The ordering matters: the range tests come
  // before the casts, so an out-of-range value is never converted.
  if (data_len < 0 || payload_len < 0 || data_len > (double)kMaxDataBytes ||
      payload_len > (double)kMaxPayloadBytes ||
      data_len != (double)(size_t)data_len ||
      payload_len != (double)(size_t)payload_len) {
    ESP_LOGE(kTag, "invalid length fields (data=%.0f payload=%.0f)", data_len,
             payload_len);
    failed_ = true;
    return false;
  }
  data_len_ = (size_t)data_len;
  payload_len_ = (size_t)payload_len;

  // Inline header `data` object is accepted on read (never written by the
  // reference); the out-of-line block, if present, wins over it.
  inline_data_json_.clear();
  const cJSON* inline_data = cJSON_GetObjectItemCaseSensitive(doc.get(), "data");
  if (cJSON_IsObject(inline_data)) {
    JsonText text(cJSON_PrintUnformatted(inline_data));
    if (text) inline_data_json_ = text.get();
  }

  have_header_ = true;
  return true;
}

bool EventDecoder::feed(const uint8_t* chunk, size_t len, EventFn on_event,
                        void* ctx) {
  if (failed_) return false;
  if (chunk && len) buf_.insert(buf_.end(), chunk, chunk + len);

  for (;;) {
    if (!have_header_) {
      if (!read_header()) break;  // need more bytes, or failed_
      if (failed_) return false;
    }

    // Need the whole data block + payload buffered before we emit.
    size_t need = data_len_ + payload_len_;
    if (buf_.size() - pos_ < need) break;  // need more bytes

    DecodedEvent ev;
    ev.type = type_;
    if (data_len_ > 0) {
      ev.data_json.assign((const char*)&buf_[pos_], data_len_);
    } else if (!inline_data_json_.empty()) {
      ev.data_json = inline_data_json_;
    }
    if (payload_len_ > 0) {
      ev.payload = &buf_[pos_ + data_len_];
      ev.payload_len = payload_len_;
    }

    bool ok = on_event(ctx, ev);

    // Consume this event's bytes.
    pos_ += need;
    have_header_ = false;
    type_.clear();
    inline_data_json_.clear();
    data_len_ = 0;
    payload_len_ = 0;

    if (!ok) return false;  // handler asked us to stop / drop the connection
  }

  compact();
  return !failed_;
}

}  // namespace espos_voice
