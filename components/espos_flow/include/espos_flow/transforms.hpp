// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — the transform library, in one include.
//
//   #include "espos_flow/flow.hpp"
//   #include "espos_flow/transforms.hpp"
//   using namespace espos::flow;
//
//   auto& raw   = g.make<Poll<float, float(*)()>>("adc", 500, read_volts);
//   auto& ohms  = g.make<DividerR2>("div", 3.3f, 1000.0f);
//   auto& level = g.make<Curve<>>("tank");
//   auto& avg   = g.make<Ema<float>>("smooth", 0.2f);
//   auto& send  = g.make<ChangeFilter<float>>("chg", 0.01f, 0.0f, 60);
//   raw >> ohms >> level >> avg >> send >> out;
//
//   level.register_config("Tank curve");   // now editable on the config page
//
// ── What is here ─────────────────────────────────────────────────────────
//
//   arithmetic.hpp  Linear, Convert, Cast, Round, Clamp, Integrator,
//                   RateOfChange, Counter
//   smoothing.hpp   MovingAverage, Median, Ema, MinMaxHold
//   filters.hpp     ChangeFilter, Throttle, Debounce, Filter, Enable,
//                   Threshold, Hysteresis, Deadband, Latch
//   timing.hpp      Repeat, Expire, Delay, RunHours, IsoTime, ParseBool,
//                   FormatBool
//   marine.hpp      Curve, DewPoint, HeatIndex, AirDensity, DividerR1/R2/Scale,
//                   Frequency, AngleOffset, WrapAngle, TankLevel, BatterySoc
//   param.hpp       ParamSet — the bridge to espos_config, used by the nodes
//                   above and available to your own
//
// docs/transforms.md is the tour, and it includes the table of where each
// SensESP transform went — including the ones that deliberately did not come
// across.
//
// ── Two things worth knowing before wiring ───────────────────────────────
//
// Everything runs on the flow task (docs/flow.md, "The one threading rule"),
// including every `register_config()` load. Nothing here blocks; the nodes
// that use a timer (Debounce, Repeat, Expire, Delay, RunHours) take one from
// the same table as Poll, bounded by ESPOS_FLOW_MAX_TIMERS.
//
// The arithmetic itself lives in `espos_formulas`, which has no IDF dependency
// and is tested on the host against published reference values. A node here is
// an id, a config page and a place in a chain around it.
#pragma once

#include "espos_flow/node.hpp"
#include "espos_flow/transforms/param.hpp"

#include "espos_flow/transforms/arithmetic.hpp"
#include "espos_flow/transforms/filters.hpp"
#include "espos_flow/transforms/marine.hpp"
#include "espos_flow/transforms/smoothing.hpp"
#include "espos_flow/transforms/timing.hpp"
