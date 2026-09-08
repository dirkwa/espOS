// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos_formulas — the marine maths, in one include.
//
//   #include "espos_formulas.hpp"
//   using namespace espos::formulas;
//
//   float depth_m = units::ft_to_m(raw_ft);
//   auto  dew     = dew_point_k(temp_k, rh);
//   auto  ohms    = divider_r2(v_out, 3.3f, 1000.0f);
//   float level   = tank_level(*ohms, 240.0f, 33.0f);
//
// Header-only and free of ESP-IDF: nothing here touches a peripheral, a task
// or NVS, so the host test in test/host/espos_formulas_test compiles the same
// code the firmware runs, and checks it against published reference values.
//
// The transform NODES that wrap these — Curve, DewPoint, Convert and the rest
// — live in espos_flow/transforms/ and are what you wire into a graph. This
// component is the arithmetic underneath, usable on its own.
//
// docs/transforms.md is the tour, including where each SensESP transform went.
#pragma once

#include "espos_formulas/curve.hpp"
#include "espos_formulas/marine.hpp"
#include "espos_formulas/units.hpp"
