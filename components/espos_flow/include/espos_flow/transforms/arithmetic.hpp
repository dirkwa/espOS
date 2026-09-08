// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the arithmetic transforms: scale, integrate, round, convert,
// clamp, rate.
//
// These are the nodes that sit directly after a sensor and turn a raw reading
// into the SI quantity Signal K wants. `Linear` is the one every installation
// uses, because no sender is exactly what its datasheet says and the two
// numbers that fix that are exactly the two a user must be able to type in.
#pragma once

#include <cmath>
#include <cstdint>

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"

namespace espos::flow {

// ────────────────────────────────────────────────────────────────── Linear
//
//   out = in * multiplier + offset
//
// SensESP's Linear, and the most-used transform in that library by a wide
// margin. Both parameters are live: `register_config()` puts them on a config
// page as `f_<id>/mul` and `f_<id>/off`, they are validated and stored in NVS,
// and a change takes effect on the next reading with no reboot.
//
// The calibration workflow this exists for: wire the sender, watch the raw
// value, measure the real quantity two ways, type the two numbers, done. That
// is a five-minute job on the dock instead of an evening with a toolchain.
template <typename T = float>
class Linear : public Symmetric<T> {
 public:
  Linear(const char* id, T multiplier = T{1}, T offset = T{0})
      : Symmetric<T>(id), mul_(multiplier), off_(offset) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(static_cast<T>(v * mul_ + off_));
  }

  T multiplier() const { return mul_; }
  T offset() const { return off_; }
  void set_multiplier(T m) { mul_ = m; }
  void set_offset(T b) { off_ = b; }

  // Put both parameters on a config page and adopt any stored values. Call
  // after espos_config_init(); a node that never calls this keeps the values
  // it was constructed with, which is what the host test relies on.
  esp_err_t register_config(const char* title = "Calibration") {
    params_.add_float("mul", "Multiplier", "", static_cast<float>(mul_));
    params_.add_float("off", "Offset", "", static_cast<float>(off_));
    esp_err_t err = params_.register_ns(this->id(), title, "Linear transform");
    if (err != ESP_OK) return err;
    float m = static_cast<float>(mul_);
    float b = static_cast<float>(off_);
    params_.load_float("mul", &m);
    params_.load_float("off", &b);
    mul_ = static_cast<T>(m);
    off_ = static_cast<T>(b);
    return ESP_OK;
  }

 private:
  T mul_;
  T off_;
  ParamSet<2> params_;
};

// ───────────────────────────────────────────────────────────────── Convert
//
// Apply a pure function to every value: the unit conversions in
// espos_formulas/units.hpp, wired as a node.
//
//   auto& kn = g.make<Convert<float, float, decltype(&units::ms_to_kn)>>(
//       "kn", units::ms_to_kn);
//
// It is a Lambda with no parameters and a name that says what it is for. The
// distinction is worth a type: a reader seeing `Convert` knows the arithmetic
// is a unit change and not a calibration, which is the difference between
// "this is correct by definition" and "this was measured".
template <typename In, typename Out, typename Fn>
class Convert : public Transform<In, Out> {
 public:
  Convert(const char* id, Fn fn) : Transform<In, Out>(id), fn_(std::move(fn)) {}

  void set(const In& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(static_cast<Out>(fn_(v)));
  }

 private:
  Fn fn_;
};

// Deduction so `Convert{"kn", units::ms_to_kn}` needs no type list.
template <typename Fn>
Convert(const char*, Fn) -> Convert<
    std::remove_cvref_t<decltype(std::declval<Fn&>()(std::declval<float>()))>,
    std::remove_cvref_t<decltype(std::declval<Fn&>()(std::declval<float>()))>,
    Fn>;

// ────────────────────────────────────────────────────────────────── Cast
//
// Change the value type without changing the value.
//
// Mostly unnecessary — the edge trampoline already converts implicitly, so
// float into a Consumer<int32_t> works with no node at all. It exists for the
// case where the conversion must be VISIBLE: a chain that deliberately
// truncates a reading to an integer count reads better with a node saying so
// than with a silent narrowing three files away.
template <typename In, typename Out>
class Cast : public Transform<In, Out> {
 public:
  explicit Cast(const char* id) : Transform<In, Out>(id) {}

  void set(const In& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(static_cast<Out>(v));
  }
};

// ────────────────────────────────────────────────────────────────── Round
//
// Round to `decimals` places. A display concern, not a data one: publishing a
// depth as 4.2 instead of 4.19999981 makes a log readable and costs nothing,
// but rounding BEFORE an average or an integral throws away information you
// paid for. Put this last in a chain, never in the middle.
class Round : public Symmetric<float> {
 public:
  Round(const char* id, int decimals = 0) : Symmetric<float>(id) {
    set_decimals(decimals);
  }

  void set(const float& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(std::round(v * scale_) / scale_);
  }

  void set_decimals(int d) {
    if (d < 0) d = 0;
    if (d > 6) d = 6;  // beyond float's ~7 significant digits it is noise
    decimals_ = d;
    scale_ = 1.0f;
    for (int i = 0; i < d; i++) scale_ *= 10.0f;
  }
  int decimals() const { return decimals_; }

 private:
  int decimals_ = 0;
  float scale_ = 1.0f;
};

// ────────────────────────────────────────────────────────────────── Clamp
//
// Limit a value to [min, max]. Not in SensESP, and it should have been: a
// sensor glitch that publishes a depth of 6000 m does more damage than a
// missing reading, because it survives into averages and rescales every graph
// that touches it.
//
// Clamp is the blunt tool — it produces a wrong-but-plausible number at the
// limit. `Threshold` (filters.hpp) is the sharp one: it drops the reading
// entirely. Prefer Threshold for data and Clamp for something that must always
// have a value, like a gauge needle or a PWM duty cycle.
template <typename T = float>
class Clamp : public Symmetric<T> {
 public:
  Clamp(const char* id, T min, T max)
      : Symmetric<T>(id), min_(min), max_(max) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(v < min_ ? min_ : (v > max_ ? max_ : v));
  }

  void set_limits(T min, T max) {
    min_ = min;
    max_ = max;
  }
  T min() const { return min_; }
  T max() const { return max_; }

 private:
  T min_;
  T max_;
};

// ───────────────────────────────────────────────────────────── Integrator
//
// A running sum: out += in * multiplier, emitted on every input.
//
// SensESP's Integrator. Two things it is genuinely for on a boat:
//
//   * a fuel or water totaliser, where the input is a flow rate and the
//     multiplier is the interval, so the sum is a volume;
//   * an amp-hour counter, where the input is current and the sum is charge.
//
// It is NOT the way to integrate a rate against real elapsed time — for that
// the multiplier would have to change per sample. `RateOfChange` is the
// inverse operation and does look at the clock; an integrator that did would
// need to know that its input arrives at a fixed rate, which the graph cannot
// promise. Feed it from a `Poll` with a known period and the multiplier IS the
// period.
//
// The total persists across a reboot when `register_config()` is used: an
// engine-hours or fuel-used figure that resets when the batteries are switched
// off is worse than none, because it looks plausible.
template <typename T = float>
class Integrator : public Symmetric<T> {
 public:
  Integrator(const char* id, T multiplier = T{1}, T initial = T{0})
      : Symmetric<T>(id), mul_(multiplier), total_(initial) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    total_ = static_cast<T>(total_ + v * mul_);
    this->emit(total_);
  }

  T total() const { return total_; }
  // Reset to a known figure: what a user does after filling the tank or
  // changing the engine. Emits, so whatever is downstream sees it at once.
  void reset(T to = T{0}) {
    total_ = to;
    this->emit(total_);
  }
  void set_multiplier(T m) { mul_ = m; }

  esp_err_t register_config(const char* title = "Total",
                            const char* unit = "") {
    params_.add_float("mul", "Multiplier", "", static_cast<float>(mul_));
    params_.add_float("total", "Total", unit, static_cast<float>(total_));
    esp_err_t err = params_.register_ns(this->id(), title, "Integrator");
    if (err != ESP_OK) return err;
    float m = static_cast<float>(mul_);
    float t = static_cast<float>(total_);
    params_.load_float("mul", &m);
    params_.load_float("total", &t);
    mul_ = static_cast<T>(m);
    total_ = static_cast<T>(t);
    return ESP_OK;
  }

  // Write the running total back to NVS. NOT called on every input: NVS is
  // flash with a finite erase budget, and a 1 Hz integrator would burn through
  // it in months. Call it on a slow Ticker (every few minutes), and once more
  // from a shutdown path if the hardware has one.
  esp_err_t persist() {
    return params_.store_float("total", static_cast<float>(total_));
  }

 private:
  T mul_;
  T total_;
  ParamSet<2> params_;
};

// ─────────────────────────────────────────────────────────── RateOfChange
//
// The derivative: how fast the input is changing, per SECOND.
//
// Not in SensESP, and the missing half of Integrator. What it is for:
//
//   * rate of turn, from a heading (with the wrap handled before it — feed it
//     an unwrapped angle or the 359-to-1 step reads as 358 deg/s);
//   * charge and discharge rate, from a battery's state of charge;
//   * the rate a bilge is filling, from a level sender, which is the
//     difference between "the pump ran" and "call for help".
//
// Emits nothing on the first value: a rate needs two samples and there is no
// honest number to produce from one. Emits nothing when two values arrive in
// the same millisecond either — the divisor would be zero, and the flow
// clock's resolution is the limit of what can be said.
template <typename T = float>
class RateOfChange : public Symmetric<T> {
 public:
  explicit RateOfChange(const char* id) : Symmetric<T>(id) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    const uint32_t now = espos_flow_now_ms();
    if (!have_prev_) {
      prev_ = v;
      prev_ms_ = now;
      have_prev_ = true;
      return;
    }
    // Modular subtraction: the flow clock is 32-bit milliseconds and wraps
    // every 49.7 days. Subtracting is correct across the wrap; comparing is
    // not.
    const uint32_t dt_ms = now - prev_ms_;
    if (dt_ms == 0) return;
    const T rate =
        static_cast<T>((v - prev_) * 1000.0f / static_cast<float>(dt_ms));
    prev_ = v;
    prev_ms_ = now;
    this->emit(rate);
  }

  void reset() { have_prev_ = false; }

 private:
  T prev_{};
  uint32_t prev_ms_ = 0;
  bool have_prev_ = false;
};

// ──────────────────────────────────────────────────────────────── Counter
//
// Count inputs and emit the count. The input's value is ignored entirely —
// what is being counted is EVENTS.
//
// Bilge pump cycles, anchor-windlass turns, tacks, MOB button presses. Pair it
// with `Filter` or `ChangeFilter` upstream to count only the events that
// matter (a rising edge rather than every sample of a level that is still
// high).
template <typename In = bool>
class Counter : public Transform<In, int32_t> {
 public:
  explicit Counter(const char* id, int32_t initial = 0)
      : Transform<In, int32_t>(id), count_(initial) {}

  void set(const In&) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    this->emit(++count_);
  }

  int32_t count() const { return count_; }
  void reset(int32_t to = 0) {
    count_ = to;
    this->emit(count_);
  }

 private:
  int32_t count_;
};

}  // namespace espos::flow
