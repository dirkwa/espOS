// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the time-shaped transforms: repeat, expire, delay, run hours,
// timestamp.
//
// These are about WHEN a value exists rather than what it is, and the pair at
// the top — Repeat and Expire — are the two halves of the same problem, which
// SensESP answered with five classes between them.
//
// The problem: a Signal K server ages a value out if nothing refreshes it, so
// a channel that genuinely only changes twice a day disappears from the
// display. But a value that is repeated forever is a lie — a depth still being
// republished ten minutes after the sounder died reads as a working sounder.
//
// So: `Repeat` keeps a slow channel alive, with a mode that says when to STOP
// (`stop_after`), and `Expire` turns a stale channel into an explicit "no
// value" rather than a stale one. Between them a display can tell "steady" from
// "dead", which is the whole point.
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"

// espos_time is optional the way espos_config is: a firmware without it still
// compiles every transform, and IsoTime reports that it has no clock.
#if ESPOS_FLOW_HAVE_TIME
#include "espos_time.h"
#endif

namespace espos::flow {

// ───────────────────────────────────────────────────────────────── Repeat
//
// Re-emit the last value on a timer. ONE node with a mode, replacing SensESP's
// Repeat, RepeatStopping, RepeatConstantRate and RepeatExpiring — which were
// four classes for one idea and, predictably, three of them were rarely used
// and none of them composed.
//
//   kAlways       — repeat forever, every `interval_ms`. For a channel that
//                   really is constant (a tank capacity, a device serial) and
//                   for which a stale value is still a true value.
//   kStopAfter    — repeat until the last real input is `max_age_ms` old, then
//                   go quiet and let the server age the channel out. This is
//                   the honest default for a SENSOR: it keeps a slow reading
//                   alive but stops lying when the sensor stops.
//   kConstantRate — emit at exactly `interval_ms` whether or not anything
//                   arrives, and pass real inputs straight through as well as
//                   restarting the clock. For a consumer that needs a fixed
//                   cadence — a display refresh, a fixed-rate log.
//
// In every mode a real input is passed through immediately; the timer is about
// what happens in the gaps.
enum class RepeatMode : uint8_t {
  kAlways,
  kStopAfter,
  kConstantRate,
};

template <typename T = float>
class Repeat : public Symmetric<T> {
 public:
  // `max_age_ms` is used by kStopAfter only; leave it 0 in the other modes.
  Repeat(const char* id, uint32_t interval_ms,
         RepeatMode mode = RepeatMode::kStopAfter, uint32_t max_age_ms = 0)
      : Symmetric<T>(id),
        interval_ms_(interval_ms),
        max_age_ms_(max_age_ms),
        mode_(mode) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    last_ = v;
    have_ = true;
    last_input_ms_ = espos_flow_now_ms();
    // kConstantRate keeps its own cadence: restarting the timer here would
    // make a burst of inputs stretch the interval, which is the one thing
    // "constant rate" promises not to do.
    if (mode_ != RepeatMode::kConstantRate) arm();
    this->emit(v);
  }

  // Start repeating. Separate from the constructor for the same reason Poll's
  // start() is: a node built in a static initialiser must not arm a timer
  // before the runtime exists.
  esp_err_t start() {
    if (mode_ == RepeatMode::kConstantRate) return arm();
    return ESP_OK;  // the other modes arm on their first input
  }

  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  uint32_t repeats() const { return repeats_; }
  void set_interval(uint32_t ms) { interval_ms_ = ms; }
  void set_max_age(uint32_t ms) { max_age_ms_ = ms; }

  esp_err_t register_config(const char* title = "Repeat") {
    params_.add_int("ivl", "Repeat every", "ms",
                    static_cast<int32_t>(interval_ms_), 0, 86400000);
    params_.add_int("age", "Stop after", "ms",
                    static_cast<int32_t>(max_age_ms_), 0, 86400000);
    esp_err_t err = params_.register_ns(this->id(), title, "Repeat");
    if (err != ESP_OK) return err;
    int32_t iv = static_cast<int32_t>(interval_ms_);
    int32_t ag = static_cast<int32_t>(max_age_ms_);
    params_.load_int("ivl", &iv);
    params_.load_int("age", &ag);
    if (iv > 0) interval_ms_ = static_cast<uint32_t>(iv);
    if (ag >= 0) max_age_ms_ = static_cast<uint32_t>(ag);
    return ESP_OK;
  }

 private:
  esp_err_t arm() {
    stop();
    return espos_flow_every(interval_ms_, &Repeat::tick, this, &timer_);
  }

  static void tick(void* self) {
    Repeat* r = static_cast<Repeat*>(self);
    if (!r->have_) return;  // nothing to repeat yet
    if (r->mode_ == RepeatMode::kStopAfter && r->max_age_ms_ > 0) {
      // Modular subtraction across the 49.7-day wrap.
      if ((espos_flow_now_ms() - r->last_input_ms_) > r->max_age_ms_) {
        r->stop();
        return;
      }
    }
    r->repeats_++;
    r->emit(r->last_);
  }

  uint32_t interval_ms_;
  uint32_t max_age_ms_;
  RepeatMode mode_;
  T last_{};
  bool have_ = false;
  uint32_t last_input_ms_ = 0;
  uint32_t repeats_ = 0;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
  ParamSet<2> params_;
};

// ───────────────────────────────────────────────────────────────── Expire
//
// Turn a value that has gone stale into an explicit "nothing".
//
// Out is `std::optional<T>`: a fresh input emits an engaged optional, and
// `max_age_ms` after the last input the node emits `std::nullopt` exactly once.
//
// This replaces SensESP's Nullable, which used a sentinel value to mean "no
// reading" — and a sentinel is a bug waiting to be averaged. `std::optional`
// makes the absence a different TYPE, so a consumer cannot accidentally treat
// it as a number: it has to write `if (v)`, and the compiler makes it.
//
// Downstream, an `sk::Output<std::optional<float>>` sends a Signal K null,
// which is how the spec says "this value is no longer available" — the display
// blanks instead of showing a stale depth.
template <typename T = float>
class Expire : public Transform<T, std::optional<T>> {
 public:
  Expire(const char* id, uint32_t max_age_ms)
      : Transform<T, std::optional<T>>(id), max_age_ms_(max_age_ms) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    expired_ = false;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
    espos_flow_after(max_age_ms_, &Expire::fire, this, &timer_);
    this->emit(std::optional<T>(v));
  }

  bool expired() const { return expired_; }
  void set_max_age(uint32_t ms) { max_age_ms_ = ms; }

  esp_err_t register_config(const char* title = "Expiry") {
    params_.add_int("age", "Expire after", "ms",
                    static_cast<int32_t>(max_age_ms_), 0, 86400000);
    esp_err_t err = params_.register_ns(this->id(), title, "Expire");
    if (err != ESP_OK) return err;
    int32_t ag = static_cast<int32_t>(max_age_ms_);
    params_.load_int("age", &ag);
    if (ag > 0) max_age_ms_ = static_cast<uint32_t>(ag);
    return ESP_OK;
  }

 private:
  static void fire(void* self) {
    Expire* e = static_cast<Expire*>(self);
    e->timer_ = ESPOS_FLOW_TIMER_NONE;
    // Once only. Repeating the null every max_age_ms would be as chatty as
    // the stale value it replaced.
    if (e->expired_) return;
    e->expired_ = true;
    e->emit(std::nullopt);
  }

  uint32_t max_age_ms_;
  bool expired_ = false;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
  ParamSet<1> params_;
};

// ────────────────────────────────────────────────────────────────── Delay
//
// Pass a value on, `ms` later.
//
// Not in SensESP. What it is for: a start interlock (do not report the engine
// as running until the oil pressure has had two seconds to come up), a
// sequenced relay, an alarm that must be true continuously rather than
// momentarily.
//
// Depth is the number of values that may be in flight at once. Each holds a
// timer, so Depth is bounded by ESPOS_FLOW_MAX_TIMERS across the whole
// firmware; a value arriving with all slots busy is DROPPED and counted, not
// queued, because a delay line that silently grows is a memory leak with a
// schedule.
template <typename T = float, std::size_t Depth = 4>
class Delay : public Symmetric<T> {
  static_assert(Depth >= 1, "a delay needs at least one slot");

 public:
  Delay(const char* id, uint32_t ms) : Symmetric<T>(id), ms_(ms) {
    for (std::size_t i = 0; i < Depth; i++) slots_[i].owner = this;
  }

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    for (std::size_t i = 0; i < Depth; i++) {
      if (slots_[i].busy) continue;
      slots_[i].value = v;
      slots_[i].busy = true;
      if (espos_flow_after(ms_, &Delay::fire, &slots_[i], &slots_[i].timer) !=
          ESP_OK) {
        slots_[i].busy = false;
        dropped_++;
      }
      return;
    }
    dropped_++;
  }

  uint32_t dropped() const { return dropped_; }
  void set_delay(uint32_t ms) { ms_ = ms; }

 private:
  struct Slot {
    Delay* owner = nullptr;
    T value{};
    bool busy = false;
    espos_flow_timer_t timer = ESPOS_FLOW_TIMER_NONE;
  };

  static void fire(void* self) {
    Slot* s = static_cast<Slot*>(self);
    s->timer = ESPOS_FLOW_TIMER_NONE;
    s->busy = false;
    s->owner->emit(s->value);
  }

  uint32_t ms_;
  Slot slots_[Depth];
  uint32_t dropped_ = 0;
};

// ─────────────────────────────────────────────────────────────── RunHours
//
// Accumulate the time an input is TRUE. SensESP's TimeCounter.
//
// This is engine hours, generator hours, watermaker hours, autopilot hours —
// the numbers a service schedule is written against, and the ones an owner is
// asked for when the boat is sold. Which is why two things about it matter
// more than the arithmetic:
//
//   * It is STORED IN SECONDS and DISPLAYED IN HOURS. The SI unit is the
//     second, so that is what goes in NVS and into a Signal K delta; the UI
//     multiplies by 1/3600 (`displayMultiplier`, docs/config.md) so the config
//     page reads "412.5 h" and a user types hours. Storing hours would mean a
//     float with three digits before the point and a resolution of seconds
//     nowhere in sight after a few thousand hours.
//   * It PERSISTS, and the total is READ-ONLY in the UI. An engine-hours
//     counter that resets on a flat battery is worse than no counter, because
//     it is believed. `persist()` writes to NVS and, crucially, is NOT called
//     on every tick — flash has a finite erase budget and a per-second write
//     would exhaust it in a season. Call it every few minutes from a Ticker.
//
// The total is read-only in config because a user who can type a new engine
// age can destroy the one record of it. Correcting it after a rebuild is a
// deliberate `set_total()` from code, not a text box.
class RunHours : public Transform<bool, float> {
 public:
  // `emit_interval_ms` is how often the running total is emitted while the
  // input is true; the total itself is exact regardless, computed from the
  // clock rather than by counting ticks.
  RunHours(const char* id, uint32_t emit_interval_ms = 60000)
      : Transform<bool, float>(id), emit_ms_(emit_interval_ms) {}

  void set(const bool& running) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    const uint32_t now = espos_flow_now_ms();
    if (running && !running_) {
      running_ = true;
      since_ms_ = now;
      espos_flow_every(emit_ms_, &RunHours::tick, this, &timer_);
    } else if (!running && running_) {
      accumulate(now);
      running_ = false;
      if (timer_ != ESPOS_FLOW_TIMER_NONE) {
        espos_flow_cancel(timer_);
        timer_ = ESPOS_FLOW_TIMER_NONE;
      }
    }
    this->emit(total_s());
  }

  // Total run time in SECONDS, including the current run if any.
  float total_s() const {
    if (!running_) return total_s_;
    return total_s_ +
           static_cast<float>(espos_flow_now_ms() - since_ms_) / 1000.0f;
  }
  // The same in hours, for a caller that wants the human number without going
  // through the config layer's display multiplier.
  float total_h() const { return total_s() / 3600.0f; }

  bool running() const { return running_; }

  // Correct the total — after an engine rebuild, or when adopting a counter
  // from an existing hour meter. Deliberately a code call: the config key is
  // read-only precisely so this cannot happen by accident from a web page.
  void set_total_s(float seconds) {
    total_s_ = seconds;
    if (running_) since_ms_ = espos_flow_now_ms();
  }

  // Register the total on a config page: read-only, seconds stored, hours
  // shown. 1/3600 is the displayMultiplier from docs/config.md.
  esp_err_t register_config(const char* title = "Run hours") {
    params_.add_float_ro("total", "Total", "s", total_s_, 1.0f / 3600.0f);
    esp_err_t err = params_.register_ns(this->id(), title, "Run hours");
    if (err != ESP_OK) return err;
    float t = total_s_;
    params_.load_float("total", &t);
    total_s_ = t;
    return ESP_OK;
  }

  // Write the total to NVS. Call from a slow Ticker (minutes), never per tick:
  // see the note at the top about flash wear.
  esp_err_t persist() {
    if (running_) accumulate(espos_flow_now_ms());
    return params_.store_float("total", total_s_);
  }

 private:
  void accumulate(uint32_t now) {
    // Modular subtraction: correct across the flow clock's 49.7-day wrap,
    // which a genset on a liveaboard will absolutely cross.
    total_s_ += static_cast<float>(now - since_ms_) / 1000.0f;
    since_ms_ = now;
  }

  static void tick(void* self) {
    RunHours* r = static_cast<RunHours*>(self);
    r->accumulate(espos_flow_now_ms());
    r->emit(r->total_s_);
  }

  uint32_t emit_ms_;
  float total_s_ = 0.0f;
  uint32_t since_ms_ = 0;
  bool running_ = false;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
  ParamSet<1> params_;
};

// ──────────────────────────────────────────────────────────────── IsoTime
//
// Emit the current wall-clock time as an ISO 8601 string, on every input.
//
// SensESP had a TimeString; this one reads espos_time, which is the device's
// ONE clock (docs/time.md) — SNTP, or a manual set, or the Signal K stream, or
// an RTC value carried through a deep sleep, ranked so a coarse source never
// overrides a good one.
//
// While the clock is unsynced it emits nothing at all. That is the important
// behaviour: espos_time returns "" rather than a plausible-looking 1970, and
// publishing 1970-01-01 as a timestamp is how an hour of data ends up in the
// wrong place in a log. No clock means no timestamp, visibly.
class IsoTime : public Transform<bool, std::string> {
 public:
  explicit IsoTime(const char* id) : Transform<bool, std::string>(id) {}

  void set(const bool&) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    emit_now();
  }

  // Emit the current time without needing an input, for a Ticker-driven chain.
  void emit_now() {
#if ESPOS_FLOW_HAVE_TIME
    char buf[ESPOS_TIME_ISO_MAX];
    if (espos_time_iso8601(buf, sizeof(buf)) == 0) return;  // unsynced
    this->emit(std::string(buf));
#endif
  }

  // Whether the device has a clock at all. A caller can publish this as its
  // own channel, which is more useful than it sounds: "the timestamps you are
  // seeing are made up" is worth knowing.
  static bool has_clock() {
#if ESPOS_FLOW_HAVE_TIME
    return espos_time_is_synced();
#else
    return false;
#endif
  }
};

// ─────────────────────────────────────────────────────── ParseBool / Format
//
// Between a bool and the strings a human or a Signal K PUT uses.
//
// ParseBool accepts what people and servers actually send: "true"/"false",
// "on"/"off", "1"/"0", "yes"/"no", in any case. It emits nothing for anything
// else — an unrecognised string is not "false", it is a message that was not
// understood, and treating it as false is how a switch panel turns everything
// off when a typo arrives.
class ParseBool : public Transform<std::string, bool> {
 public:
  explicit ParseBool(const char* id) : Transform<std::string, bool>(id) {}

  void set(const std::string& s) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    std::optional<bool> v = parse(s);
    if (v.has_value()) this->emit(*v);
  }

  static std::optional<bool> parse(const std::string& s) {
    char buf[8] = {};
    std::size_t n = s.size() < sizeof(buf) - 1 ? s.size() : sizeof(buf) - 1;
    for (std::size_t i = 0; i < n; i++) {
      char c = s[i];
      buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    if (std::strcmp(buf, "true") == 0 || std::strcmp(buf, "on") == 0 ||
        std::strcmp(buf, "1") == 0 || std::strcmp(buf, "yes") == 0) {
      return true;
    }
    if (std::strcmp(buf, "false") == 0 || std::strcmp(buf, "off") == 0 ||
        std::strcmp(buf, "0") == 0 || std::strcmp(buf, "no") == 0) {
      return false;
    }
    return std::nullopt;
  }
};

// The other direction, with the two words configurable: a display wants
// "Running"/"Stopped", a Signal K string path wants "on"/"off", and a log
// wants "true"/"false".
class FormatBool : public Transform<bool, std::string> {
 public:
  FormatBool(const char* id, const char* true_text = "true",
             const char* false_text = "false")
      : Transform<bool, std::string>(id), t_(true_text), f_(false_text) {}

  void set(const bool& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(std::string(v ? t_ : f_));
  }

 private:
  const char* t_;
  const char* f_;
};

}  // namespace espos::flow
