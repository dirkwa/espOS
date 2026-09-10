// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// SmartSwitch and BilgeSwitch — a relay a phone can operate, and a float
// switch that raises an alarm.
//
//   espos::devices::SmartSwitch light(g, {
//       .id = "deck", .relay_gpio = 10,
//       .path = "electrical.switches.deckLight.state",
//   });
//
// A local button is not a field here because it is not one decision: it needs
// a Toggle, an edge back from the relay so a PUT does not leave it one press
// behind, and a choice about long-press. `input()` is the seam --
// components/espos_sk_flow/examples/smart_switch shows the whole shape.
//
// SmartSwitch is the example that could not exist before inbound PUT, folded
// into a class: the server switches the relay, a button switches it back, and
// the relay's ACTUAL state is what gets published.
//
// The publish is not decoration. signalk-server routes a PUT to a device by
// the (path, $source) pairs it has seen that device PUBLISH, so a switch that
// never publishes is a switch the server will answer 405 for -- the request
// never even reaches the device. That is why the output edge is wired here
// and not left to the caller to remember.
#pragma once

#include "espos_devices/detail.hpp"
#include "espos_flow/graph.hpp"
#include "espos_flow/transforms.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

namespace espos::devices
{

struct SmartSwitchConfig {
    const char *id = "sw";
    int relay_gpio = -1;
    const char *path = "electrical.switches.switch.0.state";
    bool relay_active_low = false;
    bool initial = false;
};

class SmartSwitch
{
  public:
    SmartSwitch(espos::flow::Graph &g, const SmartSwitchConfig &cfg)
        : put_(g.make<espos::sk::PutHandler<bool>>(cfg.path)),
          relay_(g.make<espos::sensors::GpioOutput>(
              cfg.id, cfg.relay_gpio, cfg.relay_active_low, cfg.initial)),
          state_(g.make<espos::sk::Output<bool>>(cfg.path))
    {
        // PUT -> pin -> publish what the pin ACTUALLY did. GpioOutput emits the
        // state it drove, not the one it was asked for, and those differ exactly
        // when the pin failed to open -- which is when a dashboard must not lie.
        put_.out() >> relay_ >> state_;

        // Publish the initial state immediately. Not cosmetic: until the server
        // has seen this path from this device it will not route a PUT to it, so
        // this first delta is what makes the switch operable at all.
        relay_.set(cfg.initial);
    }

    // The relay, for a button or any other local control. Wire a toggle into
    // it and the same publish edge carries the result.
    espos::flow::Consumer<bool> &input() { return relay_; }

    espos::sk::PutHandler<bool> &put() { return put_; }
    espos::sensors::GpioOutput &relay() { return relay_; }
    espos::sk::Output<bool> &output() { return state_; }

    bool state() const { return relay_.state(); }
    esp_err_t open_error() const { return relay_.open_error(); }

  private:
    espos::sk::PutHandler<bool> &put_;
    espos::sensors::GpioOutput &relay_;
    espos::sk::Output<bool> &state_;
};

struct BilgeSwitchConfig {
    const char *id = "bilge";
    int gpio = -1;
    const char *path = "electrical.switches.bilgePump.state";
    // A float switch closes to ground, so the input is inverted and pulled up
    // by default -- the common wiring, and the one that fails safe if the
    // wire falls off (reads "not flooding" rather than a permanent alarm).
    bool invert = true;
    // Raise a Signal K notification while the switch is closed. The key is
    // short and stable because it becomes part of the path a rule keys on.
    const char *alarm_key = "bilgeHigh";
    const char *alarm_message = "Bilge water level high";
    // A float switch bobs on the water it is sensing. Without this a boat in a
    // seaway raises and clears the alarm continuously, which trains the crew to
    // ignore it -- the worst possible outcome for an alarm.
    uint32_t debounce_ms = 2000;
};

// A float switch: publishes its state, and raises a notification while wet.
class BilgeSwitch
{
  public:
    BilgeSwitch(espos::flow::Graph &g, const BilgeSwitchConfig &cfg)
        // (id, gpio, period_ms, stable_ticks, invert) -- invert is the FIFTH
        // argument, not the fourth. Passing it positionally as the fourth sets
        // the debounce tick count to 0 or 1 instead, which is the kind of
        // mistake that produces a switch that works and is subtly wrong.
        : gpio_(g.make<espos::sensors::GpioState>(cfg.id, cfg.gpio, 500,
                                                  /*stable_ticks=*/1,
                                                  cfg.invert)),
          steady_(g.make<espos::flow::Debounce<bool>>(detail::SubId(cfg.id, 'd'),
                                                      cfg.debounce_ms)),
          state_(g.make<espos::sk::Output<bool>>(cfg.path)),
          notify_(g.make<espos::sk::Notify>(cfg.alarm_key, cfg.alarm_message))
    {
        gpio_ >> steady_;
        steady_ >> state_;
        steady_ >> notify_;
    }

    esp_err_t start() { return gpio_.start(); }
    void stop() { gpio_.stop(); }

    // The debounced state, for anything else that should follow it -- a pump
    // relay, a counter, a second notification with a different threshold.
    espos::flow::Producer<bool> &wet() { return steady_; }

    espos::sensors::GpioState &gpio() { return gpio_; }
    espos::sk::Output<bool> &output() { return state_; }
    espos::sk::Notify &notify() { return notify_; }

  private:
    espos::sensors::GpioState &gpio_;
    espos::flow::Debounce<bool> &steady_;
    espos::sk::Output<bool> &state_;
    espos::sk::Notify &notify_;
};

}  // namespace espos::devices
