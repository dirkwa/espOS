// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::formulas::Curve — piecewise-linear interpolation over a sample table.
//
// This is SensESP's CurveInterpolator, and it is the single most-used thing in
// that library, because it is how a boat gets calibrated without a rebuild. A
// tank sender is a bent float arm over an irregular tank; a thermistor is an
// exponential; a fuel-level resistance curve is whatever the manufacturer felt
// like in 1987. None of them is a straight line and none of them is in a
// datasheet you have. What you do have is a bucket, a measuring jug, and an
// afternoon: fill in ten litres at a time, write down what the sender reads,
// and type the pairs into the web UI.
//
// So the sample table is DATA, not code: `espos::flow::Curve` (transforms/
// curve.hpp) exposes it as a `format: "table"` config key that the UI renders
// as a row editor and NVS keeps across reboots. This header is the maths, with
// no IDF and no storage, so the host test can hammer it.
//
// ── Endpoint behaviour, and why it is a clamp ────────────────────────────
//
// Below the first sample and above the last, this CLAMPS to the endpoint's
// output. It does not extrapolate.
//
// SensESP extrapolated from the outermost pair, and SensESP #1005 is what that
// costs: a tank curve whose first two samples were close together produced a
// near-vertical extrapolation, and an input a hair below the first sample
// yielded a wildly negative level — with a division that could reach 0/0 and
// publish a NaN. A NaN in a Signal K delta is worse than a wrong number: it
// propagates into averages, breaks JSON encoders that do not quote it, and
// disappears from a graph without an error anywhere.
//
// The physical argument agrees with the numerical one. A calibration table is
// evidence over the range it was measured; outside that range there is no
// evidence, and the honest answer is "the nearest thing I actually measured",
// not a line drawn off the edge of the world. An empty tank reads empty, not
// minus twelve litres.
//
// If a caller genuinely wants extrapolation, the right thing is to add the
// samples that describe it — which is also self-documenting.
#pragma once

#include <cstddef>
#include <optional>

namespace espos::formulas {

// One measured pair: what the sender read, and what it means.
struct CurveSample {
  float input;
  float output;
};

// Interpolate `x` over `samples[0..count)`.
//
// Preconditions the caller owns: samples must be sorted by ascending `input`.
// Sorting here would mean either mutating the caller's array or copying it,
// and the table comes from a config key that is sorted once when parsed
// (Curve::set_table does it) rather than on every value. An unsorted table is
// not undefined behaviour — it just interpolates over whichever interval it
// finds first, which looks obviously wrong the moment you plot it.
//
// Returns nullopt only for an empty table, which is the state a freshly
// registered node is in before anyone has calibrated it. That is deliberately
// distinguishable from a number: emitting 0.0 for "not calibrated yet" is how
// a tank reads empty on a boat that has never been to the fuel dock.
inline std::optional<float> curve_eval(const CurveSample* samples,
                                       std::size_t count, float x) {
  if (samples == nullptr || count == 0) return std::nullopt;
  // A single sample is a constant: one measurement says the same thing
  // everywhere, which is the clamp rule taken to its limit.
  if (count == 1) return samples[0].output;

  // Clamp, do not extrapolate — see the note at the top of this file.
  if (x <= samples[0].input) return samples[0].output;
  if (x >= samples[count - 1].input) return samples[count - 1].output;

  // Binary search for the interval [lo, lo+1) containing x. Linear scan would
  // be fine at ten samples and is not fine at the 250 the table key can hold,
  // and this runs on every reading.
  std::size_t lo = 0;
  std::size_t hi = count - 1;
  while (hi - lo > 1) {
    std::size_t mid = lo + (hi - lo) / 2;
    if (samples[mid].input <= x) {
      lo = mid;
    } else {
      hi = mid;
    }
  }

  // Duplicate inputs are a typo a user can make in the table editor (two rows
  // with the same reading, because they measured twice and typed both). The
  // interval between two of them is zero wide, so interpolating across it is
  // 0/0. This is the second half of the #1005 fix — the clamp above removed
  // the division outside the table, this removes the one inside it.
  //
  // Walk forward past a degenerate interval rather than dividing by its zero
  // width: `lo` ends up on the LAST row of the run, whose right-hand
  // neighbour is a genuinely different input, so a query beyond the duplicate
  // still interpolates properly instead of freezing at the duplicate's value.
  // The loop is bounded by the run's length and cannot pass the end, since
  // samples[count - 1] was handled by the clamp.
  while (lo + 1 < count - 1 && samples[lo + 1].input == samples[lo].input) lo++;

  const float x0 = samples[lo].input;
  const float x1 = samples[lo + 1].input;
  const float y0 = samples[lo].output;
  const float y1 = samples[lo + 1].output;

  // Still zero-width: the run runs into the final sample. Its own output is
  // the only answer that stays on the curve.
  const float dx = x1 - x0;
  if (dx == 0.0f) return y0;

  return y0 + (x - x0) * (y1 - y0) / dx;
}

// The same, for a caller that has a value to fall back on rather than an
// optional to handle — a display that would rather show something.
inline float curve_eval_or(const CurveSample* samples, std::size_t count,
                           float x, float fallback) {
  std::optional<float> v = curve_eval(samples, count, x);
  return v.has_value() ? *v : fallback;
}

// True if the table is sorted strictly ascending by input, which is what
// curve_eval() expects and what the table editor should enforce. Exposed so a
// caller can validate a table it did not build (an imported config).
inline bool curve_is_sorted(const CurveSample* samples, std::size_t count) {
  if (samples == nullptr) return count == 0;
  for (std::size_t i = 1; i < count; i++) {
    if (!(samples[i].input > samples[i - 1].input)) return false;
  }
  return true;
}

// Insertion-sort a table into ascending input order, in place. Called once
// when a table is parsed, never per value: at 250 samples the worst case is
// tens of microseconds, and a table is typed in by a human, so it arrives
// nearly sorted and this is close to linear. No allocation, which is the
// reason it is not std::sort on a copy.
inline void curve_sort(CurveSample* samples, std::size_t count) {
  if (samples == nullptr) return;
  for (std::size_t i = 1; i < count; i++) {
    CurveSample key = samples[i];
    std::size_t j = i;
    while (j > 0 && samples[j - 1].input > key.input) {
      samples[j] = samples[j - 1];
      j--;
    }
    samples[j] = key;
  }
}

}  // namespace espos::formulas
