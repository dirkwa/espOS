// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — one include for the whole graph.
//
//   #include "espos_flow/flow.hpp"
//   using namespace espos::flow;
//
//   static float read_depth();
//   static void publish(float v);
//   static float scale(float v, float m, float b);
//
//   using DepthPoll = Poll<float, float (*)()>;
//   using Cal = Lambda<float, float, decltype(&scale), float, float>;
//   using Out = Sink<float, void (*)(float)>;
//
//   static Graph g;
//
//   void app_main() {
//     ESP_ERROR_CHECK(espos_start(nullptr));
//     auto& d = g.make<DepthPoll>("depth", 1000, read_depth);
//     auto& c = g.make<Cal>("cal", scale, 1.7f, -0.16f);
//     auto& o = g.make<Out>("out", publish);
//     d >> c >> o;   // reads left to right, the way the data moves
//     d.start();
//     g.start();
//   }
//
// docs/flow.md has the full tour; the one rule to carry into it is that
// everything above runs on the flow task and nothing else may emit into it.
//
// ── Flash ────────────────────────────────────────────────────────────────
//
// The common instantiations are declared `extern template` here and defined
// once in flow_instantiations.cpp, so a firmware using float and bool nodes
// links one copy of each rather than one per translation unit. SensESP #339
// is the cautionary tale: its templates were the largest single contributor
// to image size, because every TU that included a header got its own.
//
// The list is deliberately short — the types a Signal K device actually
// publishes. A type not on it still works; it is just instantiated where it
// is used, like any other template.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "espos_flow.h"
#include "espos_flow/graph.hpp"
#include "espos_flow/node.hpp"
#include "espos_flow/nodes.hpp"

namespace espos::flow {

// The value types a Signal K device deals in. `float` is most of them (every
// SI quantity), `bool` is switches and alarms, `int32_t` is counts, `double`
// is positions, `std::string` is names and states, and std::optional<float>
// is the "sometimes there is no reading" shape.
extern template class Producer<float>;
extern template class Producer<double>;
extern template class Producer<int32_t>;
extern template class Producer<bool>;
extern template class Producer<std::string>;
extern template class Producer<std::optional<float>>;

extern template class Consumer<float>;
extern template class Consumer<double>;
extern template class Consumer<int32_t>;
extern template class Consumer<bool>;
extern template class Consumer<std::string>;
extern template class Consumer<std::optional<float>>;

extern template class Value<float>;
extern template class Value<double>;
extern template class Value<int32_t>;
extern template class Value<bool>;
extern template class Value<std::string>;

extern template class Transform<float, float>;
extern template class Transform<double, double>;
extern template class Transform<int32_t, int32_t>;
extern template class Transform<bool, bool>;
extern template class Transform<float, bool>;
extern template class Transform<float, int32_t>;

extern template class Constant<float>;
extern template class Constant<int32_t>;
extern template class Constant<bool>;

}  // namespace espos::flow
