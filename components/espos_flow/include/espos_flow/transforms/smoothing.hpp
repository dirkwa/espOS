// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — smoothing: moving average, median, exponential average,
// min/max hold.
//
// Every sensor on a boat is noisy, and the noise has two different characters
// that want two different tools:
//
//   * GAUSSIAN noise — thermal noise in an ADC, ripple on a supply. A moving
//     average is optimal for it and cheap.
//   * OUTLIERS — an ultrasonic depth sounder that occasionally sees a fish, a
//     GPS that occasionally sees a building, an ADC sample taken while the
//     windlass drew 200 A. A mean is the WRONG tool: one 6000 m depth reading
//     in a window of ten drags the average by 600 m. A median ignores it
//     entirely.
//
// Choosing between them is the difference between a depth display that is
// usable in a swell and one that spikes, so the two are separate nodes with
// this note between them rather than one node with a flag.
#pragma once

#include <cstddef>
#include <cstdint>

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"

namespace espos::flow {

// ─────────────────────────────────────────────────────── MovingAverage
//
// The mean of the last N values. SensESP's MovingAverage.
//
// A ring buffer with a running sum, so the cost per sample is one add, one
// subtract and one divide regardless of N — not O(N) like the naive form.
//
// N is a template parameter because the buffer is inside the node: no
// allocation, and the size of the graph is decided at build time like every
// other espOS table. A run-time N would mean a heap allocation in a
// constructor that may run before the heap is interesting.
//
// Before the window has filled, it averages what it has rather than emitting
// nothing. That is deliberate: a depth display that shows nothing for the
// first ten seconds after boot looks broken, and the average of three samples
// is a perfectly good depth. `full()` says whether the window is complete for
// a caller that cares.
template <std::size_t N, typename T = float>
class MovingAverage : public Symmetric<T> {
  static_assert(N >= 1, "a moving average needs at least one sample");

 public:
  explicit MovingAverage(const char* id) : Symmetric<T>(id) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (filled_ == N) {
      // Recomputing the sum from the ring every time would be O(N); keeping a
      // running sum means float error accumulates instead. Over a long run
      // that drift is real, so the sum is rebuilt whenever the ring wraps —
      // once per N samples, which is O(1) amortised and bounds the error to
      // one window's worth.
      sum_ -= ring_[head_];
    } else {
      filled_++;
    }
    ring_[head_] = v;
    sum_ += v;
    head_ = (head_ + 1) % N;
    if (head_ == 0) rebuild_sum();
    this->emit(static_cast<T>(sum_ / static_cast<double>(filled_)));
  }

  bool full() const { return filled_ == N; }
  std::size_t samples() const { return filled_; }
  void reset() {
    filled_ = 0;
    head_ = 0;
    sum_ = 0.0;
  }

  static constexpr std::size_t window = N;

 private:
  void rebuild_sum() {
    double s = 0.0;
    for (std::size_t i = 0; i < filled_; i++) s += ring_[i];
    sum_ = s;
  }

  T ring_[N] = {};
  // The accumulator is double even when T is float: summing a hundred floats
  // in float loses the low bits of every one of them, and this is the one
  // place in the graph where the extra width is worth its cost.
  double sum_ = 0.0;
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
};

// ─────────────────────────────────────────────────────────────── Median
//
// The median of the last N values. SensESP's Median.
//
// This is the outlier-resistant one — see the note at the top of the file.
// Half the window can be garbage and the output does not move.
//
// N should be ODD. With an even N the median is the mean of the two middle
// values, which reintroduces exactly the averaging-with-an-outlier problem the
// median was chosen to avoid (one wild value pulls one of the two middles).
// The static_assert says so rather than letting it be a surprise.
//
// Cost is a partial selection sort of a copy, O(N * N/2) worst case. At the
// N of 5 to 9 that a depth sounder wants that is nothing; at N of 50 it is
// noticeable per sample and MovingAverage or Ema is the better answer.
template <std::size_t N, typename T = float>
class Median : public Symmetric<T> {
  static_assert(N >= 1, "a median needs at least one sample");
  static_assert(N % 2 == 1,
                "use an odd window: an even one averages the two middle "
                "values, which is the outlier sensitivity a median exists to "
                "avoid");

 public:
  explicit Median(const char* id) : Symmetric<T>(id) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    ring_[head_] = v;
    head_ = (head_ + 1) % N;
    if (filled_ < N) filled_++;

    // Copy, then selection-sort only as far as the middle. Sorting the ring in
    // place would destroy the arrival order the ring depends on.
    T buf[N];
    for (std::size_t i = 0; i < filled_; i++) buf[i] = ring_[i];
    const std::size_t mid = filled_ / 2;
    for (std::size_t i = 0; i <= mid; i++) {
      std::size_t m = i;
      for (std::size_t j = i + 1; j < filled_; j++) {
        if (buf[j] < buf[m]) m = j;
      }
      if (m != i) {
        T t = buf[i];
        buf[i] = buf[m];
        buf[m] = t;
      }
    }
    this->emit(buf[mid]);
  }

  bool full() const { return filled_ == N; }
  std::size_t samples() const { return filled_; }
  void reset() {
    filled_ = 0;
    head_ = 0;
  }

  static constexpr std::size_t window = N;

 private:
  T ring_[N] = {};
  std::size_t head_ = 0;
  std::size_t filled_ = 0;
};

// ──────────────────────────────────────────────────────────────────── Ema
//
// Exponential moving average: out = alpha * in + (1 - alpha) * out.
//
// Not in SensESP, and it should be the default choice for most smoothing. One
// multiply-add and ONE stored float — no ring buffer, no window — against
// MovingAverage's N floats. For a device with fifteen smoothed channels that
// is the difference between 600 bytes of RAM and 60.
//
// Alpha is the responsiveness, 0..1: 1 is no smoothing at all, 0.1 is heavy.
// The rule of thumb is that alpha ≈ 2 / (N + 1) behaves like an N-sample
// moving average, so alpha 0.2 is roughly a 9-sample window.
//
// The first value is adopted as-is rather than blended toward from zero.
// Starting at zero means a temperature channel ramps up from 0 K over the
// first minute, which looks exactly like a failing sensor.
template <typename T = float>
class Ema : public Symmetric<T> {
 public:
  Ema(const char* id, float alpha = 0.2f) : Symmetric<T>(id) {
    set_alpha(alpha);
  }

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    if (!have_) {
      acc_ = static_cast<double>(v);
      have_ = true;
    } else {
      acc_ = alpha_ * static_cast<double>(v) + (1.0 - alpha_) * acc_;
    }
    this->emit(static_cast<T>(acc_));
  }

  void set_alpha(float a) {
    if (a <= 0.0f) a = 0.0001f;  // 0 would freeze the output forever
    if (a > 1.0f) a = 1.0f;
    alpha_ = a;
  }
  float alpha() const { return static_cast<float>(alpha_); }
  void reset() { have_ = false; }

  esp_err_t register_config(const char* title = "Smoothing") {
    params_.add_float("alpha", "Responsiveness", "",
                      static_cast<float>(alpha_));
    esp_err_t err =
        params_.register_ns(this->id(), title, "Exponential average");
    if (err != ESP_OK) return err;
    float a = static_cast<float>(alpha_);
    params_.load_float("alpha", &a);
    set_alpha(a);
    return ESP_OK;
  }

 private:
  double alpha_ = 0.2;
  double acc_ = 0.0;
  bool have_ = false;
  ParamSet<1> params_;
};

// ─────────────────────────────────────────────────────────── MinMaxHold
//
// Remember the extremes over a rolling window, and emit them.
//
// Not in SensESP. What it is for: max wind gust over the last ten minutes,
// which is a number every sailor wants and no instrument gives without it;
// minimum depth over the last hour, which is the one that tells you whether
// you touched; maximum engine temperature since start.
//
// The window is TIME, not a sample count — a gust figure over "the last 60
// readings" means something different at anchor than under way. The
// implementation keeps a small ring of (value, time) pairs and expires them,
// which is O(Slots) per sample; Slots is small on purpose, because a gust
// window needs resolution, not history.
//
// The output is the max; `min()` and `max()` read both. A caller that wants
// both published wires two of these, or reads the accessors from a Sink.
template <std::size_t Slots = 16, typename T = float>
class MinMaxHold : public Symmetric<T> {
  static_assert(Slots >= 2, "a hold window needs at least two slots");

 public:
  MinMaxHold(const char* id, uint32_t window_ms)
      : Symmetric<T>(id), window_ms_(window_ms) {}

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    const uint32_t now = espos_flow_now_ms();

    // The ring is a coarse histogram of the window, not every sample: each
    // slot covers window_ms/Slots and holds that bucket's own min and max.
    // That bounds the memory at 16 slots regardless of sample rate, which is
    // what lets a 10-minute gust window sit next to a 20 Hz anemometer.
    const uint32_t bucket_ms = window_ms_ / Slots ? window_ms_ / Slots : 1;
    const uint32_t bucket = now / bucket_ms;

    if (!have_) {
      for (std::size_t i = 0; i < Slots; i++) slots_[i].bucket = bucket - Slots;
      have_ = true;
    }
    Slot& s = slots_[bucket % Slots];
    if (s.bucket != bucket) {
      // This slot is from a previous lap of the ring: it has expired, so it
      // starts over with this sample rather than keeping a value from a
      // window ago.
      s.bucket = bucket;
      s.min = v;
      s.max = v;
    } else {
      if (v < s.min) s.min = v;
      if (v > s.max) s.max = v;
    }

    // Fold the live slots. A slot whose bucket is more than Slots behind is
    // outside the window and is skipped, which is how a value ages out.
    bool any = false;
    T lo{}, hi{};
    for (std::size_t i = 0; i < Slots; i++) {
      if (bucket - slots_[i].bucket >= Slots) continue;
      if (!any) {
        lo = slots_[i].min;
        hi = slots_[i].max;
        any = true;
      } else {
        if (slots_[i].min < lo) lo = slots_[i].min;
        if (slots_[i].max > hi) hi = slots_[i].max;
      }
    }
    min_ = lo;
    max_ = hi;
    this->emit(hi);
  }

  T min() const { return min_; }
  T max() const { return max_; }
  void reset() { have_ = false; }

 private:
  struct Slot {
    uint32_t bucket = 0;
    T min{};
    T max{};
  };

  Slot slots_[Slots] = {};
  uint32_t window_ms_;
  T min_{};
  T max_{};
  bool have_ = false;
};

}  // namespace espos::flow
