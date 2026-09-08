// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// smart_switch — a relay a phone can switch, and a button that switches it
// back.
//
// This is the example that could not be written before: espos_sk had no way
// to RECEIVE a PUT, so a device could report a switch but never be told to
// change it. components/espos_sk/examples/digital_switch says so in a comment.
// The whole of the inbound half is the six lines in wire() below.
//
// Three paths, and the difference between them is the point:
//
//   electrical.switches.bilge.state    a SPEC path -- no metadata, ever. The
//                                      server knows what it means.
//   sensors.bilge.pumpVoltage          OUR path -- metadata is required, and
//                                      passing units is only possible with a
//                                      sk::Meta, which is what marks it ours.
//   notifications.espos.<label>.…      raised through espos_health.
//
// No hardware is needed to build it. Nothing here was run on a board.
#include "esp_log.h"
#include "espos.h"
#include "espos_flow/flow.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

using namespace espos::flow;
using namespace espos::sensors;

namespace {

const char* TAG = "smart_switch";

// The Signal K path the switch lives on. The <id> segment ("bilge") is
// yours; "state" is a boolean the specification defines, so no metadata.
constexpr const char* kSwitch = "electrical.switches.bilge.state";

// Pins. Constructor arguments, never #defines -- see docs/sensors.md. These
// per-target choices avoid GPIO 14..19 on the ESP32-P4, which carry the SDIO
// link to the co-processor that provides WiFi on the Waveshare panels.
#if CONFIG_IDF_TARGET_ESP32P4
constexpr int kRelayPin = 22;
constexpr int kButtonPin = 23;
constexpr int kSensePin = 20;  // ADC1
#elif CONFIG_IDF_TARGET_ESP32C6
constexpr int kRelayPin = 10;
constexpr int kButtonPin = 9;  // the BOOT button on a C6 devkit
constexpr int kSensePin = 4;   // ADC1
#else
constexpr int kRelayPin = 10;
constexpr int kButtonPin = 9;
constexpr int kSensePin = 4;
#endif

Graph g;

// A divider from the pump supply to the ADC: 3.1 V at the pin is 12.4 V at
// the pump. A Lambda rather than the Linear transform, so this example
// depends on nothing but espos_flow itself.
float divider(float volts, float ratio) { return volts * ratio; }
using Divider = Lambda<float, float, decltype(&divider), float>;

// The button toggles. It is a Transform because it has to remember the state
// it is toggling -- the pin only says "pressed".
//
// It also CONSUMES the relay's state through `follow`, which is the bug this
// example would otherwise have: switch the relay on from a phone, and a
// toggle that still thought it was off would emit `true` again on the next
// press. The button would look dead for exactly one press, which is the kind
// of fault that gets blamed on the button.
class Toggle : public Transform<bool, bool> {
 public:
  explicit Toggle(const char* id) : Transform<bool, bool>(id) {}

  void set(const bool& pressed) override {
    if (pressed) emit(state_ = !state_);
  }

  // A separate consumer, so the relay's output can feed it without the
  // toggle's own output feeding back into itself.
  class Follow : public Consumer<bool> {
   public:
    explicit Follow(Toggle& t) : t_(t) {}
    using consumes_type = bool;
    void set(const bool& state) override { t_.state_ = state; }

   private:
    Toggle& t_;
  };

  Follow& follow() { return follow_; }

 private:
  bool state_ = false;
  Follow follow_{*this};
};

void wire() {
  // ── the switch, in both directions ───────────────────────────────────
  auto& put = g.make<espos::sk::PutHandler<bool>>(kSwitch);  // from the server
  auto& relay = g.make<GpioOutput>("relay", kRelayPin);      // the pin
  auto& state = g.make<espos::sk::Output<bool>>(kSwitch);  // back to the server

  // PUT -> pin -> publish what the pin ACTUALLY did. Publishing is not
  // decoration: signalk-server only routes a PUT to a device it has seen
  // publish that path, so this edge is what makes the switch operable at all.
  put.out() >> relay >> state;

  // ── the button, switching the same relay ─────────────────────────────
  auto& button = g.make<GpioChange<4>>("button", kButtonPin);
  auto& toggle = g.make<Toggle>("toggle");
  button.out() >> toggle >> relay;
  // ...and the relay tells the toggle where it ended up, so a PUT from a
  // phone does not leave the button one press behind.
  relay >> toggle.follow();

  // ── a custom path, which is why it carries metadata ──────────────────
  auto& sense = g.make<Analog>("sense", kSensePin, 2000);
  auto& volts = g.make<Divider>("divider", divider, 4.0f);
  auto& out = g.make<espos::sk::Output<float>>(
      "sensors.bilge.pumpVoltage", espos::sk::Meta{"V", "pump supply", 2000});
  sense >> volts >> out;

  sense.start();
  button.out();  // the ISR is armed by the constructor; nothing to start

  // Publish the initial state so the server knows the path exists, and so
  // this device is registered as its source. Without this first delta a PUT
  // has nowhere to go and the server answers 405 on its own.
  relay.set(false);

  ESP_LOGI(TAG, "relay GPIO %d, button GPIO %d, sense GPIO %d (%s calibration)",
           kRelayPin, kButtonPin, kSensePin,
           sense.calibrated() ? "with" : "no");
}

}  // namespace

extern "C" void app_main() {
  ESP_ERROR_CHECK(espos_start(nullptr));
  wire();
  ESP_ERROR_CHECK(g.start());
}
