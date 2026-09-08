// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// sensor_graph — the whole of a sensor firmware as four lines of wiring.
//
// This is SensESP's analog_input example written as an espOS graph, and it is
// worth reading next to two other files:
//
//   * docs/migration-from-sensesp.md, whose "planned facade" column this
//     implements — RepeatSensor is Poll, Linear is a two-parameter Lambda,
//     SKOutputFloat is a Sink.
//   * components/espos_sk/examples/analog_input, the same device written in
//     plain C. That version is not obsolete: the graph is sugar over exactly
//     those calls, and a firmware is free to use either.
//
// No ADC here, deliberately: a fake reading keeps the example about the
// wiring and lets it build for every target. Replace read_sensor() with the
// esp_adc code from the analog_input example and this is a real device.
#include <cmath>

#include "esp_log.h"
#include "espos.h"
#include "espos_flow/flow.hpp"
#include "espos_sk.h"

using namespace espos::flow;

namespace {

const char* TAG = "sensor_graph";

// A Signal K path the server already knows the units of, so nothing has to be
// declared. A path of your own needs espos_sk_declare_meta() once.
constexpr const char* kPath = "environment.inside.illuminance";

// ── the three functions the graph is made of ────────────────────────────
//
// Each is a plain function, not a capturing lambda: the node stores the
// callable BY VALUE and a function pointer is the smallest thing that can be.

// Stands in for adc_oneshot_read() + adc_cali_raw_to_voltage(). Runs on the
// flow task every period, so — like any node — it must not block. A sensor
// whose read DOES block belongs on its own task, posting into a Mailbox.
float read_sensor() {
  static float phase = 0.0f;
  phase += 0.05f;
  return 0.5f + 0.4f * std::sin(phase);  // volts, roughly
}

// SensESP's Linear, as arithmetic: the multiplier and offset are live
// parameters a config change can move without rewiring anything.
float linear(float v, float multiplier, float offset) {
  return v * multiplier + offset;
}

// The end of the chain. espos_sk_publish_number() is thread-safe, never
// blocks and works before WiFi is up — the backlog drains when the stream
// comes back, so a sink never has to know about connectivity.
void publish(float value) { espos_sk_publish_number(kPath, value); }

// Naming the instantiations keeps the wiring readable. Each is <value type,
// callable type>; the callable's type is what lets it live inside the node
// instead of in a std::function on the heap.
using Sensor = Poll<float, float (*)()>;
using Calibration = Lambda<float, float, decltype(&linear), float, float>;
using Output = Sink<float, void (*)(float)>;

// The graph owns the nodes for the life of the firmware. A static Graph is
// the ordinary shape; nothing here is ever deleted.
Graph g;

}  // namespace

extern "C" void app_main() {
  // log → config → httpd → wifi → sk → ota, in the one order that works.
  ESP_ERROR_CHECK(espos_start(nullptr));

  // ── the four lines ────────────────────────────────────────────────────
  auto& sensor = g.make<Sensor>("light", 500, read_sensor);
  auto& cal = g.make<Calibration>("cal", linear, 1.7007f, -0.165f);
  auto& out = g.make<Output>("sk", publish);

  sensor >> cal >> out;  // reads left to right, the way the data moves

  // Arm the poll, then start the loop. Both are explicit because a node
  // constructed in a static initialiser must not start a timer before the
  // runtime exists — and because "nothing runs until you say so" is easier
  // to reason about than a graph that came alive during construction.
  ESP_ERROR_CHECK(sensor.start());
  ESP_ERROR_CHECK(g.start());

  ESP_LOGI(TAG, "graph running: %zu nodes, publishing %s every 500 ms",
           g.size(), kPath);

  // app_main returns. Everything from here happens on the flow task: the
  // poll fires, the calibration transforms, the sink publishes — one task,
  // one at a time, no locks anywhere in the chain.
}
