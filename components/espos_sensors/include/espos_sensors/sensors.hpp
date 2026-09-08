// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::sensors — the hardware nodes: what a graph reads the world with.
//
//   Analog        an ADC pin, in volts            (SensESP's AnalogInput)
//   GpioState     a pin, sampled                  (DigitalInputState)
//   GpioChange    a pin, on every edge, via ISR   (DigitalInputChange)
//   GpioCounter   edges per second, ISR-counted   (DigitalInputCounter)
//   PulseCounter  edges per second, in PCNT hardware
//   GpioOutput    a relay or an LED               (a Consumer<bool>)
//   Pwm           a duty cycle                    (a Consumer<float>)
//   Ds18b20Node   a 1-Wire temperature, in kelvin
//   system::*     uptime, free heap, reset reason
//
// Every one is a node in the espos::flow graph: it emits on the flow task
// and nothing else, which is the single threading rule the whole runtime
// rests on. The two that CANNOT obey it directly -- GpioChange and
// GpioCounter, whose events arrive in an interrupt -- own a Mailbox and post
// into it, so the emit still happens on the flow task. That is the pattern to
// copy for any driver whose data arrives from somewhere else.
//
// Pins are constructor parameters. Never a #define, never a per-board
// header: a sensor library that hard-codes GPIO 4 is one that cannot be used
// on the second board. Sensible per-target pins are in the comments and in
// docs/sensors.md.
//
// Compiles with -fno-exceptions -fno-rtti. Header-only: a node that is not
// used is not compiled, so a firmware that only reads an ADC does not link
// the I2C driver.
#pragma once

#include <cstdint>
#include <optional>

#include "espos_adc.h"
#include "espos_flow/flow.hpp"
#include "espos_gpio_in.h"
#include "espos_pcnt.h"
#include "espos_pwm.h"

namespace espos::sensors {

using espos::flow::Consumer;
using espos::flow::Mailbox;
using espos::flow::NodeBase;
using espos::flow::Producer;

// ────────────────────────────────────────────────────────────────── Analog
//
// One ADC pin, read every period, emitted in VOLTS.
//
// Volts and not raw counts, because a count is meaningless without the
// attenuation and width that produced it, and because the calibration a user
// types into the web UI ("multiply by 1.7 for this divider") is only a stable
// number if what it multiplies is a voltage. Chain a Linear (or a Lambda)
// after this to get the physical quantity.
//
//   Analog tank(g, "tank", 4, 1000);   // GPIO 4, once a second
//   tank >> level >> sk::Output<float>("tanks.freshWater.0.currentLevel");
//
// A read that fails (ADC2 while the radio is busy) emits nothing rather than
// a zero: std::nullopt is how this graph says "no reading", and a fabricated
// zero on a tank level is a false empty-tank alarm.
class Analog : public NodeBase, public Producer<float> {
 public:
  // `samples` readings are averaged per emit; the SAR ADC is noisy by a few
  // LSB and 16 costs microseconds.
  Analog(const char* id, int gpio, uint32_t period_ms,
         espos_adc_atten_t atten = ESPOS_ADC_ATTEN_12DB, uint8_t samples = 16)
      : NodeBase(id), period_ms_(period_ms) {
    espos_adc_cfg_t cfg = {};
    cfg.gpio = gpio;
    cfg.atten = atten;
    cfg.samples = samples;
    open_err_ = espos_adc_open(&cfg, &adc_);
  }

  // Arm the timer. Separate from the constructor so a node built in a static
  // initialiser does not start a timer before the runtime exists.
  esp_err_t start() {
    if (open_err_ != ESP_OK) return open_err_;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) return ESP_OK;
    return espos_flow_every(period_ms_, &Analog::tick, this, &timer_);
  }

  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  // Read once, now, without waiting for the timer.
  std::optional<float> read_now() {
    if (open_err_ != ESP_OK) return std::nullopt;
    float v = 0.0f;
    if (espos_adc_read_volts(adc_, &v) != ESP_OK) return std::nullopt;
    return v;
  }

  // Whether eFuse calibration is in use. Worth logging once: a device that
  // is quietly uncalibrated looks exactly like one whose divider is wrong.
  bool calibrated() const { return espos_adc_calibrated(adc_); }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  static void tick(void* self) {
    Analog* a = static_cast<Analog*>(self);
    if (std::optional<float> v = a->read_now(); v.has_value()) a->emit(*v);
  }

  espos_adc_handle_t adc_ = nullptr;
  esp_err_t open_err_ = ESP_OK;
  uint32_t period_ms_;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

// ─────────────────────────────────────────────────────────────── GpioState
//
// A pin, sampled every period, emitted as bool. Debounced by counting: a
// level has to hold for `stable_ticks` samples before it counts, which is
// what a mechanical contact needs and what an interrupt cannot give you
// without a timer anyway.
//
//   GpioState bilge("bilge", 5, 50, 4);   // GPIO 5, 50 ms, 200 ms steady
//
// `invert` belongs in the constructor because nearly every real switch is
// active-low: the internal pull-up holds the pin high and the switch pulls it
// down, so "closed" is a 0.
class GpioState : public NodeBase, public Producer<bool> {
 public:
  GpioState(const char* id, int gpio, uint32_t period_ms = 100,
            uint8_t stable_ticks = 1, bool invert = true,
            espos_gpio_pull_t pull = ESPOS_GPIO_PULL_UP)
      : NodeBase(id),
        gpio_(gpio),
        period_ms_(period_ms),
        need_(stable_ticks ? stable_ticks : 1) {
    open_err_ = espos_gpio_in_open(gpio, pull, invert);
  }

  esp_err_t start() {
    if (open_err_ != ESP_OK) return open_err_;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) return ESP_OK;
    return espos_flow_every(period_ms_, &GpioState::tick, this, &timer_);
  }

  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  bool level() const { return espos_gpio_in_level(gpio_); }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // Emits on CHANGE, plus the very first sample. A switch that has not moved
  // is not news, and a graph that re-emits it every 100 ms turns into a
  // Signal K delta every 100 ms.
  static void tick(void* self) {
    GpioState* s = static_cast<GpioState*>(self);
    bool now = espos_gpio_in_level(s->gpio_);
    if (now != s->candidate_) {
      s->candidate_ = now;
      s->steady_ = 0;
      return;
    }
    if (s->steady_ < s->need_ && ++s->steady_ == s->need_) {
      if (!s->has_value() || now != s->get()) s->emit(now);
    }
  }

  int gpio_;
  uint32_t period_ms_;
  uint8_t need_;
  uint8_t steady_ = 0;
  bool candidate_ = false;
  esp_err_t open_err_ = ESP_OK;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

// ────────────────────────────────────────────────────────────── GpioChange
//
// Every edge, as it happens, through a Mailbox.
//
// This is the node to read if you are writing a driver of your own: the ISR
// does one thing, post the value, and the Mailbox delivers it on the flow
// task where emitting is legal. Nothing else in an interrupt is safe.
//
// Depth 8 because a bouncing contact produces a burst, and dropping the LAST
// edge of a burst is the one that matters -- it is the level the switch
// settled at.
template <std::size_t Depth = 8>
class GpioChange : public NodeBase, public Producer<bool> {
 public:
  GpioChange(const char* id, int gpio,
             espos_gpio_edge_t edge = ESPOS_GPIO_EDGE_ANY, bool invert = true,
             espos_gpio_pull_t pull = ESPOS_GPIO_PULL_UP)
      : NodeBase(id), box_(id) {
    open_err_ = espos_gpio_in_open(gpio, pull, invert);
    if (open_err_ == ESP_OK) {
      // The mailbox is the only thing the ISR touches, and `this` outlives
      // the watch: the node is owned by the graph or by a struct that lives
      // forever (see graph.hpp on why nothing is ever deleted).
      open_err_ = espos_gpio_in_watch(gpio, edge, &GpioChange::on_edge, this);
    }
  }

  // Wire downstream nodes to the mailbox, which is what actually emits.
  Producer<bool>& out() { return box_; }
  uint32_t dropped() const { return box_.dropped(); }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  // Interrupt context: post and return. No logging, no allocation, nothing
  // that blocks.
  static void on_edge(int gpio, bool level, void* arg) {
    (void)gpio;
    GpioChange* c = static_cast<GpioChange*>(arg);
    bool woken = false;
    c->box_.post_from_isr(level, &woken);
    // The flow port yields when the post woke a higher-priority task.
    (void)woken;
  }

  Mailbox<bool, Depth> box_;
  esp_err_t open_err_ = ESP_OK;
};

// ───────────────────────────────────────────────────────── PulseCounter
//
// Edges per second, over the PCNT peripheral where the chip has one and a
// GPIO ISR where it does not (the ESP32-C3). Same node either way --
// espos_pcnt.h is what hides the difference, and espos_pcnt_supported() says
// which you got.
//
// The rate is computed from the MEASURED interval, not the nominal period:
// dividing by the period assumes the poll was on time, and the first poll
// after a reconnect or a flash write is exactly when it was not.
//
//   PulseCounter rpm("rpm", 21, 1000);          // Hz on the pin
//   rpm >> divide_by_ppr >> sk::Output<float>("propulsion.main.revolutions");
class PulseCounter : public NodeBase, public Producer<float> {
 public:
  PulseCounter(const char* id, int gpio, uint32_t period_ms = 1000,
               uint32_t max_glitch_ns = 1000, bool pull_up = true,
               bool falling = false)
      : NodeBase(id), period_ms_(period_ms) {
    espos_pcnt_cfg_t cfg = {};
    cfg.gpio = gpio;
    cfg.max_glitch_ns = max_glitch_ns;
    cfg.pull_up = pull_up;
    cfg.falling = falling;
    open_err_ = espos_pcnt_open(&cfg, &pcnt_);
  }

  esp_err_t start() {
    if (open_err_ != ESP_OK) return open_err_;
    if (timer_ != ESPOS_FLOW_TIMER_NONE) return ESP_OK;
    return espos_flow_every(period_ms_, &PulseCounter::tick, this, &timer_);
  }

  void stop() {
    if (timer_ != ESPOS_FLOW_TIMER_NONE) {
      espos_flow_cancel(timer_);
      timer_ = ESPOS_FLOW_TIMER_NONE;
    }
  }

  // Total edges since boot, for an hour-meter or a chain counter. Restore a
  // saved total with set_total() after loading it from the config.
  uint64_t total() const { return espos_pcnt_total(pcnt_); }
  void set_total(uint64_t t) { espos_pcnt_set_total(pcnt_, t); }

  // True when the PCNT peripheral is doing the counting.
  static bool hardware() { return espos_pcnt_supported(); }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  static void tick(void* self) {
    PulseCounter* p = static_cast<PulseCounter*>(self);
    uint32_t count = 0;
    uint64_t us = 0;
    if (espos_pcnt_take(p->pcnt_, &count, &us) != ESP_OK) return;
    // A zero interval would divide by zero; it only happens if two ticks
    // land in the same microsecond, which the timer wheel does not do.
    if (us == 0) return;
    p->emit(static_cast<float>(count) * 1e6f / static_cast<float>(us));
  }

  espos_pcnt_handle_t pcnt_ = nullptr;
  esp_err_t open_err_ = ESP_OK;
  uint32_t period_ms_;
  espos_flow_timer_t timer_ = ESPOS_FLOW_TIMER_NONE;
};

// GpioCounter is the same node with the hardware counter declined: on a chip
// WITH a PCNT unit you may still want the ISR path (the units are a finite
// resource -- four on most chips -- and a slow reed switch does not need
// one). Same output, same units.
using GpioCounter = PulseCounter;

// ────────────────────────────────────────────────────────── GpioOutput
//
// A relay, a lamp, a solenoid: a Consumer<bool> at the end of a chain.
//
// This is half of a switch that a phone can operate -- the other half is
// sk::PutHandler, which turns an inbound Signal K PUT into the bool this
// consumes. See docs/sensors.md for the six lines that make a SmartSwitch.
class GpioOutput : public NodeBase,
                   public Consumer<bool>,
                   public Producer<bool> {
 public:
  GpioOutput(const char* id, int gpio, bool invert = false,
             bool initial = false)
      : NodeBase(id), gpio_(gpio) {
    open_err_ = espos_gpio_out_open(gpio, invert, initial);
  }

  using consumes_type = bool;

  // Setting emits too, so the graph can publish what the output actually
  // did rather than what something asked for. They differ when the pin
  // failed to open, and that is exactly when a dashboard must not lie.
  void set(const bool& on) override {
    if (open_err_ != ESP_OK) return;
    espos_gpio_out_set(gpio_, on);
    this->emit(on);
  }

  bool state() const { return espos_gpio_out_get(gpio_); }
  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  int gpio_;
  esp_err_t open_err_ = ESP_OK;
};

// ───────────────────────────────────────────────────────────────────── Pwm
//
// A duty cycle 0..1 on a pin: a dimmed cabin light, an analog gauge, a fan.
class Pwm : public NodeBase, public Consumer<float>, public Producer<float> {
 public:
  Pwm(const char* id, int gpio, uint32_t freq_hz = 5000, bool invert = false)
      : NodeBase(id) {
    espos_pwm_cfg_t cfg = {};
    cfg.gpio = gpio;
    cfg.freq_hz = freq_hz;
    cfg.invert = invert;
    open_err_ = espos_pwm_open(&cfg, &channel_);
  }

  using consumes_type = float;

  void set(const float& duty) override {
    if (open_err_ != ESP_OK) return;
    espos_pwm_set(channel_, duty);
    this->emit(
        espos_pwm_get(channel_));  // the clamped value, not the asked-for one
  }

  esp_err_t open_error() const { return open_err_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  int channel_ = -1;
  esp_err_t open_err_ = ESP_OK;
};

}  // namespace espos::sensors
