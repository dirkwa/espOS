// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the gates: what gets through, and how often.
//
// Half of a marine graph is deciding NOT to send something. A boat's link to
// its server is a WiFi association that comes and goes, the server writes
// every delta to disk, and a 20 Hz temperature channel that changes in the
// fourth decimal is a megabyte a day of nothing. These nodes are how a chain
// says "only when it matters".
//
// Four different questions, four nodes, because conflating them is what makes
// SensESP's users reach for a Lambda:
//
//   ChangeFilter  — did the value change ENOUGH?      (about the value)
//   Throttle      — has enough TIME passed?           (about the clock)
//   Debounce      — has it been STABLE long enough?   (about settling)
//   Filter        — is it plausible at all?           (about validity)
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"

namespace espos::flow {

// ──────────────────────────────────────────────────────────── ChangeFilter
//
// Pass a value only when it differs from the last PASSED value by at least
// `min_delta` — with two escape hatches SensESP got right and that are easy to
// get wrong when writing this by hand.
//
//   * `max_delta` — a change LARGER than this is dropped too. That is the
//     glitch filter: a depth that jumps 40 m between two 1 Hz samples did not
//     happen, and passing it means the plotter draws a spike and the shallow
//     alarm fires in open water. 0 disables the upper bound.
//   * `max_skips` — after this many consecutive drops, pass the value anyway.
//     Without it, a channel that is genuinely steady stops publishing
//     entirely, the server ages the value out, and the display goes blank on a
//     boat where nothing is wrong. This is the setting people discover by
//     losing their tank levels overnight.
//
// The first value always passes: there is nothing to compare it against, and
// a chain that starts silent is a chain that looks broken.
template <typename T = float>
class ChangeFilter : public Symmetric<T> {
 public:
  ChangeFilter(const char* id, T min_delta, T max_delta = T{0},
               uint32_t max_skips = 0)
      : Symmetric<T>(id),
        min_delta_(min_delta),
        max_delta_(max_delta),
        max_skips_(max_skips) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (!have_) {
      pass(v);
      return;
    }
    const T d = static_cast<T>(
        std::fabs(static_cast<double>(v) - static_cast<double>(last_)));
    const bool too_small = d < min_delta_;
    const bool too_large = max_delta_ > T{0} && d > max_delta_;

    if (!too_small && !too_large) {
      pass(v);
      return;
    }

    skips_++;
    // The keepalive applies to the "did not change enough" case only. A value
    // that was rejected as a GLITCH must never be forced through by the skip
    // counter: that would publish the one reading the filter exists to stop,
    // just later. SensESP counted both together; this is a deliberate
    // difference and the reason max_skips is safe to turn on.
    if (too_small && max_skips_ > 0 && skips_ >= max_skips_) pass(v);
  }

  uint32_t skipped() const { return skips_; }
  void set_min_delta(T d) { min_delta_ = d; }
  void set_max_delta(T d) { max_delta_ = d; }
  void set_max_skips(uint32_t n) { max_skips_ = n; }

  esp_err_t register_config(const char* title = "Change filter",
                            const char* unit = "") {
    params_.add_float("min", "Minimum change", unit,
                      static_cast<float>(min_delta_));
    params_.add_float("max", "Maximum change (0 = off)", unit,
                      static_cast<float>(max_delta_));
    params_.add_int("skips", "Send anyway after N skips", "",
                    static_cast<int32_t>(max_skips_), 0, 100000);
    esp_err_t err = params_.register_ns(this->id(), title, "Change filter");
    if (err != ESP_OK) return err;
    float mn = static_cast<float>(min_delta_);
    float mx = static_cast<float>(max_delta_);
    int32_t sk = static_cast<int32_t>(max_skips_);
    params_.load_float("min", &mn);
    params_.load_float("max", &mx);
    params_.load_int("skips", &sk);
    min_delta_ = static_cast<T>(mn);
    max_delta_ = static_cast<T>(mx);
    max_skips_ = static_cast<uint32_t>(sk < 0 ? 0 : sk);
    return ESP_OK;
  }

 private:
  void pass(const T& v) {
    last_ = v;
    have_ = true;
    skips_ = 0;
    this->emit(v);
  }

  T last_{};
  T min_delta_;
  T max_delta_;
  uint32_t max_skips_;
  uint32_t skips_ = 0;
  bool have_ = false;
  ParamSet<3> params_;
};

// ─────────────────────────────────────────────────────────────── Throttle
//
// Pass at most one value per `min_interval_ms`, dropping the rest.
//
// The rate limiter, and the right tool when the input rate is the problem
// rather than the input's stability: an N2K bus delivering position at 10 Hz
// into a server that wants 1 Hz. It keeps the LATEST value in each interval
// and discards the earlier ones — a stale value passed on schedule would be
// worse than a fresh one passed a little late.
//
// Note this drops rather than buffers. A chain that must not lose values wants
// a Mailbox and a slower consumer, not a Throttle.
template <typename T = float>
class Throttle : public Symmetric<T> {
 public:
  Throttle(const char* id, uint32_t min_interval_ms)
      : Symmetric<T>(id), interval_ms_(min_interval_ms) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    const uint32_t now = espos_flow_now_ms();
    // Modular subtraction, correct across the 49.7-day wrap of the flow clock.
    if (have_ && (now - last_ms_) < interval_ms_) {
      dropped_++;
      return;
    }
    last_ms_ = now;
    have_ = true;
    this->emit(v);
  }

  uint32_t dropped() const { return dropped_; }
  void set_interval(uint32_t ms) { interval_ms_ = ms; }

 private:
  uint32_t interval_ms_;
  uint32_t last_ms_ = 0;
  uint32_t dropped_ = 0;
  bool have_ = false;
};

// ─────────────────────────────────────────────────────────────── Debounce
//
// Emit only after the value has been UNCHANGED for `stable_ms`.
//
// SensESP's DebounceTemplate. The classic case is a mechanical switch, whose
// contacts chatter for a few milliseconds and which would otherwise register
// as a dozen presses; the marine case is a float switch in a bilge, which does
// exactly the same thing on every wave.
//
// It works by DEFERRING: a value arriving schedules an emit `stable_ms` later,
// and a different value arriving before then cancels and reschedules. So the
// output always lags the input by at least stable_ms, and a value that never
// settles is never emitted at all. Both are the point.
//
// Bounded by the timer table (ESPOS_FLOW_MAX_TIMERS): each Debounce holds at
// most one pending timer, taken when a change arrives and released when it
// fires.
template <typename T = bool>
class Debounce : public Symmetric<T> {
 public:
  Debounce(const char* id, uint32_t stable_ms)
      : Symmetric<T>(id), stable_ms_(stable_ms) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    // Same value as the one already pending: leave the timer alone. Restarting
    // it would mean a switch held closed while something else chatters never
    // settles.
    if (pending_ && v == candidate_) return;
    candidate_ = v;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
    pending_ = true;
    espos_flow_after(stable_ms_, &Debounce::fire, this, &timer_);
  }

  void set_stable_ms(uint32_t ms) { stable_ms_ = ms; }
  uint32_t stable_ms() const { return stable_ms_; }

  esp_err_t register_config(const char* title = "Debounce") {
    params_.add_int("ms", "Stable for", "ms", static_cast<int32_t>(stable_ms_),
                    0, 60000);
    esp_err_t err = params_.register_ns(this->id(), title, "Debounce");
    if (err != ESP_OK) return err;
    int32_t ms = static_cast<int32_t>(stable_ms_);
    params_.load_int("ms", &ms);
    if (ms >= 0) stable_ms_ = static_cast<uint32_t>(ms);
    return ESP_OK;
  }

 private:
  static void fire(void* self) {
    Debounce* d = static_cast<Debounce*>(self);
    d->timer_ = ESPOS_FLOW_TIMER_NONE;
    d->pending_ = false;
    // Only emit on an actual change of the settled value: a switch that
    // bounces and comes back to where it was is not an event.
    if (d->have_ && d->settled_ == d->candidate_) return;
    d->settled_ = d->candidate_;
    d->have_ = true;
    d->emit(d->settled_);
  }

  uint32_t stable_ms_;
  T candidate_{};
  T settled_{};
  bool pending_ = false;
  bool have_ = false;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
  ParamSet<1> params_;
};

// ───────────────────────────────────────────────────────────────── Filter
//
// Pass a value only when `pred(value)` is true. The general escape hatch, and
// the honest way to say "this reading is not real": a GPS speed of 300 kn, a
// temperature of -200 C, a depth of zero from a sounder that lost bottom.
//
//   auto& plausible = g.make<Filter<float, bool(*)(float)>>(
//       "plaus", [](float d) { return d > 0.1f && d < 200.0f; });
//
// Dropping beats clamping for data: a clamped 200 m depth is indistinguishable
// from a real one downstream, whereas a dropped reading simply does not appear
// and the server ages the channel out — which is the truth.
template <typename T, typename Pred>
class Filter : public Symmetric<T> {
 public:
  Filter(const char* id, Pred pred)
      : Symmetric<T>(id), pred_(std::move(pred)) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (pred_(v)) {
      this->emit(v);
    } else {
      rejected_++;
    }
  }

  uint32_t rejected() const { return rejected_; }

 private:
  Pred pred_;
  uint32_t rejected_ = 0;
};

// ───────────────────────────────────────────────────────────────── Enable
//
// A gate with a switch: pass values while enabled, drop them while not.
//
// SensESP's Enable. The switch is set from outside — a config key, a REST
// call, another chain — via `enable()` / `disable()`, or by wiring a bool
// producer into `control()`.
//
//   sw.output >> tank.gate.control();   // publish tank level only under way
//
// Unlike Filter the decision does not depend on the value, which is why the
// two are separate: one is about the data, the other about the situation.
template <typename T = float>
class Enable : public Symmetric<T> {
 public:
  Enable(const char* id, bool enabled = true)
      : Symmetric<T>(id), enabled_(enabled) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (enabled_) this->emit(v);
  }

  void enable() { enabled_ = true; }
  void disable() { enabled_ = false; }
  void set_enabled(bool e) { enabled_ = e; }
  bool enabled() const { return enabled_; }

  // A Consumer<bool> that drives the gate, so the switch can be a chain of its
  // own rather than a call from somewhere. Returned by reference and lives in
  // the node, so wiring it is safe for the node's lifetime.
  class Control : public Consumer<bool> {
   public:
    using consumes_type = bool;
    void set(const bool& on) override {
      if (owner) owner->enabled_ = on;
    }
    Enable* owner = nullptr;
  };

  Control& control() {
    control_.owner = this;
    return control_;
  }

 private:
  bool enabled_;
  Control control_;
};

// ────────────────────────────────────────────────────────────── Threshold
//
// Is the value inside (or outside) a range? In to bool, out as an alarm.
//
// SensESP's FloatThreshold / IntThreshold, generalised. `in_range` picks the
// polarity: true emits "the value is between min and max", false emits "it is
// outside them", which is the shape an alarm wants — engine temperature
// outside 60..95 C is the condition, not inside.
//
// Bounds are inclusive. It emits on EVERY input, not only on a change; put a
// ChangeFilter after it if the consumer is a publisher. That is deliberate:
// an alarm consumer usually wants to know the state is still true.
template <typename T = float>
class Threshold : public Transform<T, bool> {
 public:
  Threshold(const char* id, T min, T max, bool in_range = true)
      : Transform<T, bool>(id), min_(min), max_(max), in_range_(in_range) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    const bool inside = (v >= min_ && v <= max_);
    this->emit(in_range_ ? inside : !inside);
  }

  void set_limits(T min, T max) {
    min_ = min;
    max_ = max;
  }
  T min() const { return min_; }
  T max() const { return max_; }

  esp_err_t register_config(const char* title = "Threshold",
                            const char* unit = "", float display_mul = 0.0f) {
    params_.add_float("min", "Minimum", unit, static_cast<float>(min_), "",
                      display_mul);
    params_.add_float("max", "Maximum", unit, static_cast<float>(max_), "",
                      display_mul);
    esp_err_t err = params_.register_ns(this->id(), title, "Threshold");
    if (err != ESP_OK) return err;
    float mn = static_cast<float>(min_);
    float mx = static_cast<float>(max_);
    params_.load_float("min", &mn);
    params_.load_float("max", &mx);
    min_ = static_cast<T>(mn);
    max_ = static_cast<T>(mx);
    return ESP_OK;
  }

 private:
  T min_;
  T max_;
  bool in_range_;
  ParamSet<2> params_;
};

// ───────────────────────────────────────────────────────────── Hysteresis
//
// Two thresholds and a memory: the node that keeps a relay from chattering.
//
// SensESP's Hysteresis. The state goes HIGH when the input rises above
// `upper`, and LOW again only when it falls below `lower` — not when it drops
// back under `upper`. The gap between the two is the whole point.
//
// A bilge pump wired through a single threshold at 50 mm runs in bursts of a
// tenth of a second as the water sloshes around the switch point, which wears
// the pump out and flattens the battery. With `lower` at 20 mm and `upper` at
// 60 mm it runs once, properly, and stops.
//
// The same applies to a fridge compressor, an engine-temperature alarm, and a
// low-battery cutout. If you are switching anything physical, this node rather
// than Threshold.
//
// Out is any type, so it can emit a bool, an int for a display code, or a
// string state. Before the first input it emits nothing: there is no state
// until something has been measured.
template <typename In = float, typename Out = bool>
class Hysteresis : public Transform<In, Out> {
 public:
  Hysteresis(const char* id, In lower, In upper, Out low_value, Out high_value)
      : Transform<In, Out>(id),
        lower_(lower),
        upper_(upper),
        low_(low_value),
        high_(high_value) {}

  void set(const In& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    bool next = state_;
    if (v >= upper_) {
      next = true;
    } else if (v <= lower_) {
      next = false;
    } else if (!have_) {
      // First value, inside the dead band: there is no previous state to
      // hold, and guessing wrong would switch something on. Assume LOW —
      // the safe side for a pump, a heater and a cutout alike.
      next = false;
    }

    const bool changed = !have_ || next != state_;
    state_ = next;
    have_ = true;
    // Emits only on a change of state. Hysteresis exists to reduce switching,
    // so re-announcing the same state on every sample would defeat it; a
    // consumer that needs the current state can read get().
    if (changed) this->emit(state_ ? high_ : low_);
  }

  bool state() const { return state_; }
  void set_limits(In lower, In upper) {
    lower_ = lower;
    upper_ = upper;
  }

  esp_err_t register_config(const char* title = "Hysteresis",
                            const char* unit = "", float display_mul = 0.0f) {
    params_.add_float("lower", "Switch off below", unit,
                      static_cast<float>(lower_), "", display_mul);
    params_.add_float("upper", "Switch on above", unit,
                      static_cast<float>(upper_), "", display_mul);
    esp_err_t err = params_.register_ns(this->id(), title, "Hysteresis");
    if (err != ESP_OK) return err;
    float lo = static_cast<float>(lower_);
    float hi = static_cast<float>(upper_);
    params_.load_float("lower", &lo);
    params_.load_float("upper", &hi);
    lower_ = static_cast<In>(lo);
    upper_ = static_cast<In>(hi);
    return ESP_OK;
  }

 private:
  In lower_;
  In upper_;
  Out low_;
  Out high_;
  bool state_ = false;
  bool have_ = false;
  ParamSet<2> params_;
};

// ──────────────────────────────────────────────────────────────── Deadband
//
// Pass values, but hold the output steady while the input stays within
// `width` of the last output.
//
// Not in SensESP, and different from ChangeFilter in a way worth the second
// node: ChangeFilter DROPS a small change, so nothing is emitted at all;
// Deadband emits the OLD value, so the output keeps flowing at full rate but
// stops jittering.
//
// That distinction is the difference between a display that goes stale and a
// display that sits still. Use ChangeFilter before a publisher (save
// bandwidth); use Deadband before a gauge, a relay or a helm display (stop the
// last digit flickering).
template <typename T = float>
class Deadband : public Symmetric<T> {
 public:
  Deadband(const char* id, T width) : Symmetric<T>(id), width_(width) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (!have_) {
      held_ = v;
      have_ = true;
      this->emit(held_);
      return;
    }
    const double d =
        std::fabs(static_cast<double>(v) - static_cast<double>(held_));
    if (d > static_cast<double>(width_)) held_ = v;
    this->emit(held_);
  }

  void set_width(T w) { width_ = w; }
  T width() const { return width_; }

 private:
  T width_;
  T held_{};
  bool have_ = false;
};

// ────────────────────────────────────────────────────────────────── Latch
//
// Once the input is true, stay true until reset.
//
// Not in SensESP. What it is for: an alarm that must not clear itself. A high
// bilge level that lasted two seconds at 03:00 and then drained is exactly the
// event you need to know about in the morning, and a plain Threshold forgets
// it the moment the water goes down.
//
// `reset()` clears it, which is what an acknowledge button does.
class Latch : public Symmetric<bool> {
 public:
  Latch(const char* id, bool latch_on = true)
      : Symmetric<bool>(id), latch_on_(latch_on) {}

  void set(const bool& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (v == latch_on_) latched_ = true;
    this->emit(latched_ ? latch_on_ : v);
  }

  bool latched() const { return latched_; }
  // Clear the latch and emit the released state, so an acknowledge is visible
  // downstream immediately rather than at the next reading.
  void reset() {
    latched_ = false;
    this->emit(!latch_on_);
  }

 private:
  bool latch_on_;
  bool latched_ = false;
};

}  // namespace espos::flow
