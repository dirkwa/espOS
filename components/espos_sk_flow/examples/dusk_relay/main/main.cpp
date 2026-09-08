// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// dusk_relay — a deck light that follows the boat's own light sensor.
//
// This is components/espos_sk/examples/listener_relay written as a graph, and
// the two are worth reading side by side because the C version is not obsolete
// and its comments explain what this one hides.
//
// What the C version has to build by hand:
//
//   * a FreeRTOS queue, because the subscription callback runs on the stream
//     task and must not block — call espos_sk from there and the device
//     deadlocks against its own socket;
//   * a worker task to drain that queue, with a stack size to choose;
//   * the hysteresis, as two comparisons against a `bool` that has to be
//     initialised to something before the first reading arrives.
//
// Here the queue is inside Listener (it posts into a Mailbox), the worker task
// is the flow loop every graph already has, and the hysteresis is a node. What
// is left is the four lines in app_main that say what this device IS.
//
// The hysteresis matters more than it looks. A single threshold at 50 lux
// makes a cloud passing over the sensor at 50.2, 49.8, 50.1 lux switch the
// deck light on and off three times. Two thresholds with a gap between them —
// on below 50, off above 100 — mean nothing happens inside that band, so the
// light changes state at dusk and at dawn and not in between.
//
// No hardware is needed to build this. Nothing here was run on a board.
#include "esp_log.h"
#include "espos.h"
#include "espos_flow/flow.hpp"
#include "espos_flow/transforms.hpp"
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

using namespace espos::flow;
using namespace espos::sensors;

namespace {

const char* TAG = "dusk_relay";

// The relay's pin. Kept off the pins that carry the ESP32-C6 SDIO link on the
// Waveshare P4 panels, which is why this is not one number for every target.
#if CONFIG_IDF_TARGET_ESP32P4
constexpr int kRelayGpio = 22;
#else
constexpr int kRelayGpio = 10;
#endif

// What we listen to: whichever device on the boat publishes ambient light.
// A spec path, so the server owns its metadata and we declare none.
constexpr const char* kLightPath = "environment.outside.illuminance";

// What we report: the switch this device owns. Also a spec path — the
// <id> segment is ours to choose, "state" is defined to be a boolean.
constexpr const char* kStatePath = "electrical.switches.deckLight.state";

// Dusk and dawn, in lux. The gap between them is the hysteresis band.
constexpr float kOnBelow = 50.0f;
constexpr float kOffAbove = 100.0f;

// One second is the rate we ASK the server for. It is a hint about how often
// we want the value, not a local filter: asking for ten times a second would
// cost the whole boat's bandwidth to switch a light that changes twice a day.
constexpr uint32_t kSubscribePeriodMs = 1000;

Graph g;

}  // namespace

extern "C" void app_main() {
  // log → config → httpd → wifi → sk → ota, in the one order that works.
  ESP_ERROR_CHECK(espos_start(nullptr));

  // ── the graph ─────────────────────────────────────────────────────────
  //
  // Read it as a sentence: the light level from the server, through a
  // hysteresis, drives a relay, and what the relay actually did is published
  // back.
  auto& light =
      g.make<espos::sk::Listener<float>>(kLightPath, kSubscribePeriodMs);
  // The value arguments are inverted on purpose: Hysteresis emits `high` above
  // the upper limit and `low` below the lower one, and a deck light is ON when
  // it is DARK. So high (bright) maps to false and low (dark) to true. One
  // consequence to know about if you call dusk.register_config(): the built-in
  // parameter labels read "Switch on above" and "Switch off below", which are
  // the wrong way round for a light. Give this node its own labels rather than
  // confusing whoever edits them.
  auto& dusk = g.make<Hysteresis<float, bool>>("dusk", kOnBelow, kOffAbove,
                                               /*low=*/true, /*high=*/false);
  auto& relay = g.make<GpioOutput>("relay", kRelayGpio);
  auto& state = g.make<espos::sk::Output<bool>>(kStatePath);

  // Listener emits through its mailbox, so the chain starts at out().
  light.out() >> dusk >> relay >> state;

  ESP_ERROR_CHECK(g.start());

  // Two things worth checking rather than assuming, because both fail
  // quietly: a subscription the server never accepted, and a pin that would
  // not open. Publishing a switch state that no pin is following is worse
  // than not publishing it, because a dashboard then shows a light that is
  // not on.
  if (!light.subscribed()) {
    ESP_LOGW(TAG,
             "not subscribed to %s yet — it is sent when the stream connects",
             kLightPath);
  }
  if (relay.open_error() != ESP_OK) {
    ESP_LOGE(TAG, "GPIO %d did not open: %s", kRelayGpio,
             esp_err_to_name(relay.open_error()));
  }

  ESP_LOGI(
      TAG,
      "%zu nodes: %s -> on below %.0f lux, off above %.0f -> GPIO %d -> %s",
      g.size(), kLightPath, kOnBelow, kOffAbove, kRelayGpio, kStatePath);

  // app_main returns and everything happens on the flow task: the stream
  // task posts each value into the listener's mailbox, the loop takes it,
  // and the chain runs to completion before the next one is looked at. No
  // locks anywhere in it.
  //
  // Note what is NOT here: nothing republishes on a timer. Signal K keeps the
  // last value it was sent, so a switch that changes twice a day is published
  // twice a day. The C version republishes every 10 s because it was written
  // before this device could be asked for its state; if you want that
  // behaviour, a Ticker into the same Output gives it.
}
