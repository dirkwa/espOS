// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::sk — the Signal K end of the graph.
//
//   Output<T>(path)            publish a value          (SensESP's SKOutput)
//   Output<T>(path, Meta{…})   publish a CUSTOM path, and declare its meta
//   Listener<T>(path)          receive a value from the server
//   PutRequest<T>(path)        ask the server to change something
//   PutHandler<T>(path)        let the server change something HERE
//   Notify(key)                raise/clear a device condition
//   NetRssi / IpAddress        what the network says about itself
//
// ── The one rule this file is built around ──────────────────────────────
//
// Never send metadata for a path in the Signal K specification. The server
// already knows that navigation.speedOverGround is metres per second; a
// device that declares it anyway can only get it wrong, and a device that
// gets it wrong makes every dashboard on the boat wrong.
//
// So there is no `units` argument. A units string cannot be passed without
// constructing a `Meta`, and constructing a Meta says "this path is mine,
// nobody else knows what it means" -- which is exactly when metadata is
// correct. The rule is not documented and hoped for; it is unspeakable.
//
//   spec path, so no meta is possible:
//     sk::Output<float> sog("navigation.speedOverGround");
//   our own path, so meta is required to say what it means:
//     sk::Output<float> pv("sensors.solar.0.voltage", sk::Meta{"V"});
//
// ── Threading ───────────────────────────────────────────────────────────
//
// Output, PutRequest and Notify are called from the graph, on the flow task,
// and the espos_sk publish calls they make are thread-safe and never block.
//
// Listener and PutHandler receive from the STREAM task, and neither emits
// there: both post into a Mailbox so the emit happens on the flow task like
// every other node. That is why they hold one.
#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#include "espos_flow/flow.hpp"
#include "espos_health.h"
#include "espos_net.h"
#include "espos_sk.h"

namespace espos::sk {

using espos::flow::Consumer;
using espos::flow::Mailbox;
using espos::flow::NodeBase;
using espos::flow::Producer;

// ──────────────────────────────────────────────────────────────────── Meta
//
// The metadata for a path this device invented. Its presence is what marks a
// path as custom (see the rule above), which is why it has no default
// constructor that means "nothing".
//
// `units` is a Signal K unit string -- the SI one for the quantity: "V",
// "A", "K", "m", "m/s", "Hz", "ratio", "Pa". Not "°C" and not "kn": the
// convention is that the wire carries SI and the display converts.
struct Meta {
  const char* units;
  const char* description = nullptr;
  // Added as "timeout" (in seconds, 2.5x this) when non-zero: how long a
  // consumer should treat the last value as current. The one meta field a
  // device genuinely owns, because only it knows how often it publishes.
  uint32_t period_ms = 0;
};

namespace detail {

// A node id is 12 characters (NodeBase truncates); a Signal K path is far
// longer. The TAIL is what distinguishes two nodes in one firmware --
// "navigation.speedOverGround" and "navigation.speedThroughWater" share
// their first 12 characters exactly -- so keep the last segment, not the
// first. The returned pointer is into the caller's path, which is a string
// literal for the whole life of the firmware.
inline const char* short_id(const char* path) {
  const char* dot = std::strrchr(path, '.');
  return dot && dot[1] ? dot + 1 : path;
}

// Value -> JSON text. One overload per type the graph carries; each is the
// smallest correct spelling, and none of them allocates.
inline void to_json(char* buf, std::size_t n, float v) {
  std::snprintf(buf, n, "%.6g", (double)v);
}
inline void to_json(char* buf, std::size_t n, double v) {
  std::snprintf(buf, n, "%.9g", v);
}
inline void to_json(char* buf, std::size_t n, int32_t v) {
  std::snprintf(buf, n, "%ld", (long)v);
}
inline void to_json(char* buf, std::size_t n, bool v) {
  std::snprintf(buf, n, "%s", v ? "true" : "false");
}

// A JSON string, with the five characters JSON forbids escaped. A sensor
// that reports a name or a state can contain a quote, and one unescaped
// quote makes the whole delta unparseable -- the server drops the frame and
// every value in it, not just this one.
inline void to_json(char* buf, std::size_t n, const std::string& v) {
  std::size_t o = 0;
  if (n < 3) return;
  buf[o++] = '"';
  for (char c : v) {
    if (o + 7 >= n) break;  // room for a \u00XX plus the closing quote
    switch (c) {
      case '"':
        buf[o++] = '\\';
        buf[o++] = '"';
        break;
      case '\\':
        buf[o++] = '\\';
        buf[o++] = '\\';
        break;
      case '\n':
        buf[o++] = '\\';
        buf[o++] = 'n';
        break;
      case '\r':
        buf[o++] = '\\';
        buf[o++] = 'r';
        break;
      case '\t':
        buf[o++] = '\\';
        buf[o++] = 't';
        break;
      default:
        if ((unsigned char)c < 0x20) {
          o += std::snprintf(buf + o, n - o, "\\u%04x",
                             (unsigned)(unsigned char)c);
        } else {
          buf[o++] = c;
        }
    }
  }
  buf[o++] = '"';
  buf[o] = '\0';
}

// JSON text -> value, for what arrives from the server. Returns nullopt when
// the text is not that type, so a Listener on a path whose value turns out
// to be an object emits nothing instead of a zero.
template <typename T>
std::optional<T> from_json(const char* json);

template <>
inline std::optional<float> from_json<float>(const char* j) {
  if (!j || !*j) return std::nullopt;
  char* end = nullptr;
  float v = std::strtof(j, &end);
  if (end == j) return std::nullopt;
  return v;
}
template <>
inline std::optional<double> from_json<double>(const char* j) {
  if (!j || !*j) return std::nullopt;
  char* end = nullptr;
  double v = std::strtod(j, &end);
  if (end == j) return std::nullopt;
  return v;
}
template <>
inline std::optional<int32_t> from_json<int32_t>(const char* j) {
  if (!j || !*j) return std::nullopt;
  char* end = nullptr;
  long v = std::strtol(j, &end, 10);
  if (end == j) return std::nullopt;
  return (int32_t)v;
}
template <>
inline std::optional<bool> from_json<bool>(const char* j) {
  if (!j) return std::nullopt;
  if (std::strncmp(j, "true", 4) == 0) return true;
  if (std::strncmp(j, "false", 5) == 0) return false;
  // Signal K switches are booleans, but 0/1 turns up from hand-written
  // clients and from a REST PUT typed into a browser.
  if (j[0] == '1') return true;
  if (j[0] == '0') return false;
  return std::nullopt;
}
template <>
inline std::optional<std::string> from_json<std::string>(const char* j) {
  if (!j) return std::nullopt;
  std::string s(j);
  // Strip the JSON quotes; anything else (a number, an object) is passed
  // through as its text, which is what a string listener on a mixed path
  // most usefully sees.
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
    return s.substr(1, s.size() - 2);
  return s;
}

}  // namespace detail

// ────────────────────────────────────────────────────────────────── Output
//
// The end of a chain: publish every value it is given.
//
// Publishing is thread-safe, never blocks, and works before the network is
// up -- values are batched and buffered, and the backlog drains when the
// stream connects. A sink never has to know about connectivity.
template <typename T>
class Output : public NodeBase, public Consumer<T> {
 public:
  // A path in the Signal K specification. No metadata is sent, ever.
  explicit Output(const char* path)
      : NodeBase(detail::short_id(path)), path_(path) {}

  // A path of this device's own, with the metadata that explains it. The
  // declaration is reconciled with the server on every connect and the
  // server's copy wins if a user edited it there.
  Output(const char* path, const Meta& meta)
      : NodeBase(detail::short_id(path)), path_(path) {
    char json[160];
    if (meta.description && *meta.description) {
      std::snprintf(json, sizeof(json),
                    "{\"units\":\"%s\",\"description\":\"%s\"}", meta.units,
                    meta.description);
    } else {
      std::snprintf(json, sizeof(json), "{\"units\":\"%s\"}", meta.units);
    }
    espos_sk_declare_meta(path, json, meta.period_ms);
  }

  using consumes_type = T;

  void set(const T& v) override {
    char buf[64];
    detail::to_json(buf, sizeof(buf), v);
    espos_sk_publish_json(path_, buf);
  }

  const char* path() const { return path_; }

 private:
  const char* path_;
};

// std::optional<T> publishes JSON null when disengaged: the Signal K way of
// saying "this sensor has nothing right now", which is different from zero
// and different from stale.
template <typename T>
class Output<std::optional<T>> : public NodeBase,
                                 public Consumer<std::optional<T>> {
 public:
  explicit Output(const char* path)
      : NodeBase(detail::short_id(path)), path_(path) {}
  Output(const char* path, const Meta& meta)
      : NodeBase(detail::short_id(path)), path_(path) {
    char json[160];
    std::snprintf(json, sizeof(json), "{\"units\":\"%s\"}", meta.units);
    espos_sk_declare_meta(path, json, meta.period_ms);
  }

  using consumes_type = std::optional<T>;

  void set(const std::optional<T>& v) override {
    if (!v.has_value()) {
      espos_sk_publish_json(path_, "null");
      return;
    }
    char buf[64];
    detail::to_json(buf, sizeof(buf), *v);
    espos_sk_publish_json(path_, buf);
  }

 private:
  const char* path_;
};

// ──────────────────────────────────────────────────────────────── Listener
//
// A value the SERVER sends, as a node. Anything published on the boat can
// drive this device: a depth from the sounder, a wind angle, a switch state
// another device owns.
//
// The subscription callback runs on the stream task, so it posts into a
// Mailbox and the emit happens on the flow task like everything else.
template <typename T, std::size_t Depth = 4>
class Listener : public NodeBase, public Producer<T> {
 public:
  // period_ms is the server-side rate hint, not a local filter: it is how
  // often the server is asked to send, and asking for more than you need
  // costs the whole boat's bandwidth.
  Listener(const char* path, uint32_t period_ms = 1000)
      : NodeBase(detail::short_id(path)), box_(detail::short_id(path)) {
    handle_ = espos_sk_subscribe(path, period_ms, &Listener::on_update, this);
  }

  // Downstream nodes wire to the mailbox, which is what emits.
  Producer<T>& out() { return box_; }
  bool subscribed() const { return handle_ > 0; }
  uint32_t dropped() const { return box_.dropped(); }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // Stream task. Convert, post, return -- no blocking, and nothing that
  // calls back into espos_sk.
  static void on_update(const espos_sk_update_t* u, void* arg) {
    Listener* l = static_cast<Listener*>(arg);
    if (!u->value_json) return;  // a meta item, not a value
    if (std::optional<T> v = detail::from_json<T>(u->value_json); v.has_value())
      l->box_.post(*v);
  }

  Mailbox<T, Depth> box_;
  int handle_ = -1;
};

// ────────────────────────────────────────────────────────────── PutHandler
//
// The node that makes a switch operable from a phone.
//
// A PUT arrives from the server, this node emits the requested value on the
// flow task, and whatever is wired downstream (a GpioOutput, say) applies
// it. The server is answered COMPLETED 200 as soon as the value is accepted
// into the mailbox.
//
//   sk::PutHandler<bool> req("electrical.switches.bilge.state");
//   req.out() >> relay >> sk::Output<bool>("electrical.switches.bilge.state");
//
// The publish at the end is not decoration: the server only routes a PUT to
// a device it has SEEN publish that path, so a switch that never publishes
// its state can never be operated. Wiring the output back through the graph
// is what registers this device as the path's source.
template <typename T, std::size_t Depth = 4>
class PutHandler : public NodeBase, public Producer<T> {
 public:
  explicit PutHandler(const char* path)
      : NodeBase(detail::short_id(path)), box_(detail::short_id(path)) {
    err_ = espos_sk_put_handler_register(path, &PutHandler::on_put, this);
  }

  Producer<T>& out() { return box_; }
  bool registered() const { return err_ == ESP_OK; }
  uint32_t requests() const { return requests_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // Stream task. Returning ESP_OK answers COMPLETED 200; the value is
  // applied a moment later on the flow task.
  //
  // Answering "accepted" rather than "applied" is deliberate: the
  // alternative is to block the stream task until the flow task has run the
  // chain, and a slow consumer would then stall every other frame on the
  // connection. A PUT that is refused outright still fails loudly, which is
  // the case a client can actually act on.
  static esp_err_t on_put(const char* path, const char* value_json, void* arg) {
    (void)path;
    PutHandler* h = static_cast<PutHandler*>(arg);
    std::optional<T> v = detail::from_json<T>(value_json);
    if (!v.has_value()) return ESP_ERR_INVALID_ARG;  // answered 400
    h->requests_++;
    if (h->box_.post(*v) != ESP_OK)
      return ESP_FAIL;  // answered 502: the graph is behind
    return ESP_OK;
  }

  Mailbox<T, Depth> box_;
  esp_err_t err_ = ESP_FAIL;
  uint32_t requests_ = 0;
};

// ────────────────────────────────────────────────────────────── PutRequest
//
// The other direction: this device asks the SERVER to change something. A
// button here that switches a relay on another device.
template <typename T>
class PutRequest : public NodeBase, public Consumer<T> {
 public:
  explicit PutRequest(const char* path)
      : NodeBase(detail::short_id(path)), path_(path) {}

  using consumes_type = T;

  void set(const T& v) override {
    char buf[64];
    detail::to_json(buf, sizeof(buf), v);
    // Fire and forget: the response is logged by espos_sk, and a node that
    // waited for it would block the flow task for the round trip.
    espos_sk_put(path_, buf, nullptr, nullptr);
  }

 private:
  const char* path_;
};

// ────────────────────────────────────────────────────────────────── Notify
//
// A bool that raises or clears a device condition. espos_health records it,
// and espos_sk publishes it as a Signal K notification under
// notifications.espos.<label>.<key>.
//
// Level-triggered and idempotent: raising the same state twice sends one
// notification, so wiring a comparator straight into this is fine.
class Notify : public NodeBase, public Consumer<bool> {
 public:
  // `key` is a short stable identifier ("lowOil", "bilgeHigh"), not a
  // sentence: it becomes part of the path, and the path is what a rule keys
  // on. `message` is the human half and may change freely.
  Notify(const char* key, const char* message,
         espos_health_state_t state = ESPOS_HEALTH_ALARM)
      : NodeBase(key), key_(key), message_(message), state_(state) {}

  using consumes_type = bool;

  void set(const bool& raised) override {
    espos_health_report(key_, raised ? state_ : ESPOS_HEALTH_NORMAL, message_);
  }

 private:
  const char* key_;
  const char* message_;
  espos_health_state_t state_;
};

// ────────────────────────────────────────────────── NetRssi / IpAddress
//
// What the network says about itself, as graph values.
//
// RSSI in dBm; 0 on a device whose route is not WiFi (Ethernet, or the P4
// talking through its co-processor). Signal strength is the first thing to
// look at when a device drops out at one end of the boat.
inline std::optional<int32_t> net_rssi() {
  espos_net_status_t st;
  if (espos_net_get_status(&st) != ESP_OK) return std::nullopt;
  if (st.rssi == 0)
    return std::nullopt;  // not a WiFi route: no reading, not "0 dBm"
  return static_cast<int32_t>(st.rssi);
}

inline std::optional<std::string> net_ip() {
  espos_net_status_t st;
  if (espos_net_get_status(&st) != ESP_OK) return std::nullopt;
  if (!st.ip[0]) return std::nullopt;
  return std::string(st.ip);
}

using NetRssi = espos::flow::Poll<int32_t, std::optional<int32_t> (*)()>;
using IpAddress =
    espos::flow::Poll<std::string, std::optional<std::string> (*)()>;

}  // namespace espos::sk
