// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the node library: the pieces every sensor program is made of.
//
//   Poll<T>      read something every N ms          (SensESP's RepeatSensor)
//   Ticker       fire every N ms, no value          (onRepeat)
//   Constant<T>  a value emitted once at start
//   Lambda<...>  arbitrary arithmetic, variadic     (SensESP #903)
//   Sink<T>      the end of a chain
//   Join<Ts...>  the ONE multi-input node           (SensESP #899)
//   Mailbox<T>   the way in from another task or an ISR
//
// Every callable here is stored BY VALUE, not in a std::function: a captured
// lambda lives inside the node, there is no allocation, and nothing to
// dangle. That is why Lambda takes its callable as a template parameter.
//
// SensESP #571 — "a transform that sometimes has nothing to say" — is a
// callable returning std::optional<Out>: engaged emits, disengaged does not.
// No sentinel values, no separate "valid" output.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "espos_flow.h"
#include "espos_flow/node.hpp"

namespace espos::flow {

// ──────────────────────────────────────────────────────────────────── Poll
//
// Call `fn` every `period_ms` and emit what it returns. This is the idiom
// every SensESP add-on library is built on (RepeatSensor<float>(500, read)),
// and the reason it is first in this file.
//
// `fn` may return T (always emits) or std::optional<T> (emits only when
// engaged) — a sensor that is still warming up, or a read that failed, simply
// produces nothing rather than a fabricated zero.
//
// The timer is armed by start(), which the Graph does not call for you: a
// node constructed in a static initialiser must not start a timer before the
// runtime exists. Call start() from your wiring function, or let
// Graph::start() do it — see Poll::autostart below.
template <typename T, typename Fn>
class Poll : public NodeBase, public Producer<T> {
 public:
  Poll(const char* id, uint32_t period_ms, Fn fn)
      : NodeBase(id), fn_(std::move(fn)), period_ms_(period_ms) {}

  // Arm the timer. Idempotent; ESP_ERR_NO_MEM when the timer table is full,
  // which is a loud log line from espos_flow rather than a silent no-op.
  esp_err_t start() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) return ESP_OK;
    return espos_flow_every(period_ms_, &Poll::tick, this, &timer_);
  }

  // Stop polling. The node keeps its last value; start() resumes.
  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  // Read once, now, on the calling task. What start() does on every tick;
  // useful to prime a chain before the first period elapses.
  void read_now() { tick(this); }

  uint32_t period_ms() const { return period_ms_; }

  // Change the period. Takes effect on the next tick after a restart, which
  // is what a config change wants: `poll.set_period(cfg.period_ms)`.
  void set_period(uint32_t ms) {
    if (ms == 0 || ms == period_ms_) return;
    period_ms_ = ms;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      stop();
      start();
    }
  }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  static void tick(void* self) {
    Poll* p = static_cast<Poll*>(self);
    ESPOS_FLOW_ASSERT_TASK(p->id());
    // Both shapes in one node: `-> T` and `-> std::optional<T>`. The
    // branch is a compile-time one, so the optional costs nothing when
    // it is not used.
    using R = decltype(p->fn_());
    if constexpr (std::is_same_v<R, std::optional<T>>) {
      if (std::optional<T> v = p->fn_(); v.has_value()) p->emit(*v);
    } else {
      p->emit(static_cast<T>(p->fn_()));
    }
  }

  Fn fn_;
  uint32_t period_ms_;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

// Deduction so `Poll{"depth", 1000, read}` works without naming Fn. The value
// type comes from what the callable returns, with std::optional unwrapped.
template <typename Fn>
Poll(const char*, uint32_t, Fn)
    -> Poll<std::remove_cvref_t<decltype(std::declval<Fn&>()())>, Fn>;

// ────────────────────────────────────────────────────────────────── Ticker
//
// A clock with no value: emits its own tick count every period. For a chain
// that wants a heartbeat rather than a reading — a keepalive, a display
// refresh, a "still alive" publish.
class Ticker : public NodeBase, public Producer<uint32_t> {
 public:
  Ticker(const char* id, uint32_t period_ms)
      : NodeBase(id), period_ms_(period_ms) {}

  esp_err_t start() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) return ESP_OK;
    return espos_flow_every(period_ms_, &Ticker::tick, this, &timer_);
  }

  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  uint32_t count() const { return count_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  static void tick(void* self) {
    Ticker* t = static_cast<Ticker*>(self);
    t->emit(++t->count_);
  }

  uint32_t period_ms_;
  uint32_t count_ = 0;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

// ──────────────────────────────────────────────────────────────── Constant
//
// A value that never changes, emitted once when emit_now() is called (or by
// Graph wiring at start). For a calibration constant that feeds a Join, or a
// unit conversion factor a Lambda multiplies by.
template <typename T>
class Constant : public NodeBase, public Producer<T> {
 public:
  Constant(const char* id, const T& v) : NodeBase(id), v_(v) {}

  void emit_now() { this->emit(v_); }
  const T& value() const { return v_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  T v_;
};

// ──────────────────────────────────────────────────────────────────── Sink
//
// The end of a chain: a consumer that calls your function and produces
// nothing. `sk::Output<T>` will be one of these; so is "set a GPIO", "draw a
// number", "log it".
template <typename T, typename Fn>
class Sink : public NodeBase, public Consumer<T> {
 public:
  Sink(const char* id, Fn fn) : NodeBase(id), fn_(std::move(fn)) {}

  using consumes_type = T;

  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(id());
    fn_(v);
  }

 private:
  Fn fn_;
};

// ────────────────────────────────────────────────────────────────── Lambda
//
// The general transform: N inputs, one output, arbitrary arithmetic, plus
// named parameters the config layer can drive.
//
// SensESP #903 asked for a variadic LambdaTransform because the fixed 0..3
// parameter overloads ran out and each shape was a separate class. Here the
// parameters are a std::tuple<Ps...> stored in the node, exposed by index, and
// the callable is invoked as fn(input, params...).
//
// Returning std::optional<Out> emits only when engaged (SensESP #571): the
// idiomatic way to write "ignore readings outside the plausible range" without
// inventing a magic value that a downstream node has to know about.
//
//   auto& cal = g.make<Lambda<float, float, float, float>>(
//       "cal", [](float v, float m, float b) { return v * m + b; }, 1.7f,
//       -0.16f);
//   cal.param<0>() = new_multiplier;   // from a config change
template <typename In, typename Out, typename Fn, typename... Ps>
class Lambda : public Transform<In, Out> {
 public:
  Lambda(const char* id, Fn fn, Ps... params)
      : Transform<In, Out>(id),
        fn_(std::move(fn)),
        params_(std::move(params)...) {}

  void set(const In& v) override {
    ESPOS_FLOW_ASSERT_TASK(this->id());
    apply(v, std::index_sequence_for<Ps...>{});
  }

  // Live parameters. Writing one does not emit — the next input does, with
  // the new value. That is deliberate: a config change should not fabricate
  // a reading out of a stale input.
  template <std::size_t I>
  auto& param() {
    return std::get<I>(params_);
  }
  template <std::size_t I>
  const auto& param() const {
    return std::get<I>(params_);
  }

  static constexpr std::size_t param_count = sizeof...(Ps);

 private:
  template <std::size_t... I>
  void apply(const In& v, std::index_sequence<I...>) {
    using R = decltype(fn_(v, std::get<I>(params_)...));
    if constexpr (std::is_same_v<R, std::optional<Out>>) {
      if (std::optional<Out> r = fn_(v, std::get<I>(params_)...); r.has_value())
        this->emit(*r);
    } else {
      this->emit(static_cast<Out>(fn_(v, std::get<I>(params_)...)));
    }
  }

  Fn fn_;
  std::tuple<Ps...> params_;
};

// ──────────────────────────────────────────────────────────────────── Join
//
// The ONE multi-input node. SensESP #899 observed that Join, Zip and
// connect_from were three answers to one question, each with its own age and
// completeness rules; this is the answer.
//
// Join<float, bool>("j", 2000, Policy::kAll) has two typed slots, in<0>() and
// in<1>(), and emits std::tuple<float, bool>:
//
//   * Policy::kAll  — emit when every slot has a value AND every value is
//     younger than max_age_ms. This is "combine a depth and a heading into one
//     message" and it must not pair a fresh depth with a heading from a minute
//     ago.
//   * Policy::kAny  — emit on every input, carrying whatever the other slots
//     last held, provided they are all still within max_age_ms. This is
//     "publish the whole state whenever any part of it changes".
//
// max_age_ms of 0 disables the age rule entirely: values never go stale, which
// is what you want for something that genuinely changes once a day.
enum class Policy : uint8_t {
  kAny,  // emit on any input, if the others are present and fresh
  kAll,  // emit only when every slot has been filled since the last emit
};

template <typename... Ts>
class Join : public NodeBase, public Producer<std::tuple<Ts...>> {
  static_assert(sizeof...(Ts) >= 2,
                "Join needs at least two inputs; a single input needs no join");

 public:
  Join(const char* id, uint32_t max_age_ms, Policy policy)
      : NodeBase(id), max_age_ms_(max_age_ms), policy_(policy) {
    init_slots(std::index_sequence_for<Ts...>{});
  }

  // One typed slot per input. `producer.connect_to(join.in<0>())`.
  template <std::size_t I>
  auto& in() {
    return std::get<I>(slots_);
  }

  static constexpr std::size_t arity = sizeof...(Ts);

  // How many slots currently hold a value at all (regardless of age).
  std::size_t filled() const {
    return count_filled(std::index_sequence_for<Ts...>{});
  }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // A slot is a Consumer<T> that records the value, its arrival time and,
  // for kAll, whether it has been fed since the last emit.
  template <std::size_t I, typename T>
  class Slot : public Consumer<T> {
   public:
    using consumes_type = T;

    void set(const T& v) override {
      value = v;
      present = true;
      fresh_for_all = true;
      at_ms = espos_flow_now_ms();
      if (owner) owner->on_input();
    }

    T value{};
    bool present = false;
    bool fresh_for_all = false;
    uint32_t at_ms = 0;
    Join* owner = nullptr;
  };

  template <std::size_t... I>
  void init_slots(std::index_sequence<I...>) {
    ((std::get<I>(slots_).owner = this), ...);
  }

  template <std::size_t... I>
  std::size_t count_filled(std::index_sequence<I...>) const {
    return (static_cast<std::size_t>(std::get<I>(slots_).present) + ...);
  }

  // Fresh means "arrived within max_age_ms of now", on the modular clock:
  // subtract, never compare. 0 disables the rule.
  template <std::size_t I>
  bool slot_fresh(uint32_t now) const {
    const auto& sl = std::get<I>(slots_);
    if (!sl.present) return false;
    if (max_age_ms_ == 0) return true;
    return (now - sl.at_ms) <= max_age_ms_;
  }

  template <std::size_t... I>
  bool all_fresh(uint32_t now, std::index_sequence<I...>) const {
    return (slot_fresh<I>(now) && ...);
  }

  template <std::size_t... I>
  bool all_fed(std::index_sequence<I...>) const {
    return (std::get<I>(slots_).fresh_for_all && ...);
  }

  template <std::size_t... I>
  void clear_fed(std::index_sequence<I...>) {
    ((std::get<I>(slots_).fresh_for_all = false), ...);
  }

  template <std::size_t... I>
  std::tuple<Ts...> gather(std::index_sequence<I...>) const {
    return std::tuple<Ts...>(std::get<I>(slots_).value...);
  }

  void on_input() {
    using Idx = std::index_sequence_for<Ts...>;
    uint32_t now = espos_flow_now_ms();
    if (!all_fresh(now, Idx{})) return;

    if (policy_ == Policy::kAll) {
      if (!all_fed(Idx{})) return;
      // Every slot must be fed again before the next emit: kAll is
      // "one output per complete set of inputs", not "one output per
      // input once the set is complete".
      clear_fed(Idx{});
    }
    this->emit(gather(Idx{}));
  }

  // Index the slot types so each Slot is a distinct class even when two
  // inputs have the same T — two Consumer<float> base classes in one object
  // would otherwise be ambiguous.
  template <typename Seq>
  struct SlotsFor;
  template <std::size_t... I>
  struct SlotsFor<std::index_sequence<I...>> {
    using type = std::tuple<Slot<I, Ts>...>;
  };

  typename SlotsFor<std::index_sequence_for<Ts...>>::type slots_;
  uint32_t max_age_ms_;
  Policy policy_;
};

// ───────────────────────────────────────────────────────────────── Mailbox
//
// The only supported way into a graph from another task or an interrupt.
//
// post() copies the value into the node and asks the flow loop to emit it;
// the emit happens on the flow task, so everything downstream obeys the one
// threading rule without knowing where the value came from.
//
// The value is held in a small ring, not a single slot: two posts arriving
// before the loop runs must both be delivered, and an ISR that posts twice in
// a burst is the ordinary case, not the exception. Depth is a template
// parameter because the right depth is a property of the source (a button
// wants 2, a pulse counter wants 16), not of the firmware.
template <typename T, std::size_t Depth = 4>
class Mailbox : public NodeBase, public Producer<T> {
  static_assert(Depth >= 1, "a Mailbox needs at least one slot");

 public:
  explicit Mailbox(const char* id) : NodeBase(id) {}

  // A post hands the loop a callback holding `this`, and the loop may not
  // have run it yet when the mailbox dies -- a Mailbox with block scope, or
  // one owned by a node that is torn down. Running that callback afterwards
  // reads a destroyed object and emits into a destroyed producer.
  //
  // There is no way to withdraw a queued post (the flow mailbox is a plain
  // FreeRTOS queue of {callback, argument} and cancelling by argument would
  // have to walk it), so the object marks itself dead instead and deliver()
  // becomes a no-op. The flag is atomic because the loop runs on another
  // task, and release/acquire so that a delivery which observes `live_` as
  // true also sees the ring contents that were published before it.
  //
  // This only makes the stale delivery harmless. A Mailbox still must not
  // outlive its graph, and the usual arrangement -- Graph::make<T>(), owned
  // for the firmware's lifetime -- never reaches this path at all.
  ~Mailbox() { live_.store(false, std::memory_order_release); }

  Mailbox(const Mailbox&) = delete;
  Mailbox& operator=(const Mailbox&) = delete;

  // From any task. Never blocks. Returns ESP_ERR_NO_MEM when the ring is
  // full (the loop is behind) or the flow mailbox is, and the value is
  // dropped — see espos_flow.h on why dropping beats blocking.
  esp_err_t post(const T& v) {
    uint32_t seq;
    if (!claim(&seq)) return ESP_ERR_NO_MEM;
    ring_[seq % Depth] = v;
    commit(seq);
    esp_err_t err = espos_flow_post(&Mailbox::deliver, this);
    /* A refused flow post leaves the value in the ring; the next accepted
     * post drains it, since deliver() takes one entry per call and the
     * ring is FIFO. Nothing is lost that the ring still holds. */
    return err;
  }

  // From an interrupt. `hp_task_woken` is forwarded to
  // espos_flow_post_from_isr(); yield from the ISR when it comes back true.
  esp_err_t post_from_isr(const T& v, bool* hp_task_woken = nullptr) {
    uint32_t seq;
    if (!claim(&seq)) return ESP_ERR_NO_MEM;
    ring_[seq % Depth] = v;
    commit(seq);
    return espos_flow_post_from_isr(&Mailbox::deliver, this, hp_task_woken);
  }

  // Posts refused because the ring was full. A growing number means the
  // producer is faster than the loop.
  uint32_t dropped() const { return dropped_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // Three monotonically increasing sequence numbers, never decremented, so
  // a reservation is never handed back and a concurrent claim can never be
  // confused by one. `claimed_` reserves, `ready_` publishes, `read_`
  // consumes; a slot is only visible to the loop once ready_ passes it.
  //
  // This is the one place in the C++ layer that needs atomics: post() is
  // callable from any task and post_from_isr() from an interrupt, where a
  // mutex is not an option.
  bool claim(uint32_t* seq) {
    uint32_t c = claimed_.load(std::memory_order_relaxed);
    for (;;) {
      if (c - read_.load(std::memory_order_acquire) >= Depth) {
        dropped_++;
        return false;
      }
      if (claimed_.compare_exchange_weak(c, c + 1, std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
        *seq = c;
        return true;
      }
    }
  }

  // Publish slot `seq`. Writers finish out of order under contention, so a
  // writer waits for its predecessor before advancing the ready mark —
  // otherwise the loop could read a slot whose value is still being
  // written. Depth is small and posts are rare; the spin is bounded by the
  // number of writers actually in flight.
  void commit(uint32_t seq) {
    uint32_t expected = seq;
    while (!ready_.compare_exchange_weak(expected, seq + 1,
                                         std::memory_order_acq_rel,
                                         std::memory_order_relaxed)) {
      expected = seq;
    }
  }

  static void deliver(void* self) {
    Mailbox* m = static_cast<Mailbox*>(self);
    // The mailbox was destroyed after this delivery was queued: there is
    // nothing left to emit into, and the object under `self` is gone.
    if (!m->live_.load(std::memory_order_acquire)) return;
    uint32_t r = m->read_.load(std::memory_order_relaxed);
    // Nothing published yet: a post whose flow-post was accepted before a
    // slower writer committed. The value is not lost — the next delivery
    // takes it.
    if (r == m->ready_.load(std::memory_order_acquire)) return;
    T v = m->ring_[r % Depth];
    m->read_.store(r + 1, std::memory_order_release);
    m->emit(v);
  }

  T ring_[Depth] = {};
  std::atomic<uint32_t> claimed_{0};
  std::atomic<uint32_t> ready_{0};
  std::atomic<uint32_t> read_{0};
  uint32_t dropped_ = 0;
  std::atomic<bool> live_{true};
};

}  // namespace espos::flow
