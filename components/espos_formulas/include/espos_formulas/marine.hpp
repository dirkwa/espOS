// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::formulas — the marine arithmetic: the domain knowledge that is the
// actual reason to pick a marine framework over writing C.
//
// Every function here is pure: floats in, floats out, no IDF, no state, no
// allocation. That is what lets the host test check them against PUBLISHED
// reference values — Arden Buck's own table, NOAA's heat-index chart, a
// resistive divider worked out by hand — rather than against themselves.
// A formula that only agrees with its own previous output is not tested.
//
// Signal K units throughout: kelvin, pascals, ratio 0..1, m3, Hz, radians.
// Where a formula is defined in Celsius (dew point, heat index — they are, in
// their sources), the conversion happens inside and the boundary stays SI.
#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "espos_formulas/curve.hpp"
#include "espos_formulas/units.hpp"

namespace espos::formulas {

// ────────────────────────────────────────────────────────── dew point
//
// The Arden Buck equation (Buck, "New Equations for Computing Vapor Pressure
// and Enhancement Factor", J. Appl. Meteorol. 20 (1981) 1527, in the 1996
// revised coefficients). More accurate than the Magnus form SensESP used, and
// the difference matters at the temperatures a cabin actually sees: Magnus
// drifts by a few tenths of a kelvin below freezing, which is precisely where
// you care whether the condensation on the inside of the coachroof is about to
// become ice.
//
// The gamma form:
//
//   gamma(T, RH) = ln(RH) + (b - T/d) * (T / (c + T))
//   Td           = c * gamma / (b - gamma)
//
// with T in Celsius and RH as a ratio 0..1. Coefficients for water above
// freezing; the sub-zero (over ice) set differs, and this uses the water set
// throughout because that is what a hygrometer in a cabin is measuring —
// supercooled droplets, not frost.
inline constexpr float kBuckB = 18.678f;
inline constexpr float kBuckC = 257.14f;  // degrees C
inline constexpr float kBuckD = 234.5f;   // degrees C

// Dew point in kelvin from temperature in kelvin and relative humidity as a
// ratio 0..1 (Signal K's `environment.*.relativeHumidity` is a ratio, not a
// percentage — see units::percent_to_ratio).
//
// Returns nullopt when RH is at or below zero: ln(0) is -inf and the dew point
// of perfectly dry air is not a number, it is "there is no dew point". SensESP
// returned -inf here and published it. An RH above 1 is clamped to 1 rather
// than refused: a cheap sensor reading 1.02 in fog is saturated, not broken,
// and the dew point of saturated air is the air temperature.
inline std::optional<float> dew_point_k(float temp_k, float rh_ratio) {
  if (!(rh_ratio > 0.0f)) return std::nullopt;
  if (rh_ratio > 1.0f) rh_ratio = 1.0f;

  const float t = units::k_to_c(temp_k);
  const float gamma =
      std::log(rh_ratio) + (kBuckB - t / kBuckD) * (t / (kBuckC + t));
  // gamma == b would be a division by zero; it needs RH near zero AND an
  // absurd temperature, so this is a guard rather than a case.
  const float denom = kBuckB - gamma;
  if (denom == 0.0f) return std::nullopt;
  return units::c_to_k(kBuckC * gamma / denom);
}

// Saturation vapour pressure over water, in pascals, from temperature in
// kelvin. The other half of Arden Buck, and what AirDensity needs:
//
//   es(T) = 611.21 * exp((18.678 - T/234.5) * (T / (257.14 + T)))   [T in C]
inline float saturation_vapour_pressure_pa(float temp_k) {
  const float t = units::k_to_c(temp_k);
  return 611.21f * std::exp((kBuckB - t / kBuckD) * (t / (kBuckC + t)));
}

// Actual vapour pressure: saturation times relative humidity.
inline float vapour_pressure_pa(float temp_k, float rh_ratio) {
  if (rh_ratio < 0.0f) rh_ratio = 0.0f;
  if (rh_ratio > 1.0f) rh_ratio = 1.0f;
  return saturation_vapour_pressure_pa(temp_k) * rh_ratio;
}

// ───────────────────────────────────────────────────────── heat index
//
// The NOAA/Rothfusz regression (National Weather Service Technical Attachment
// SR 90-23), which is the equation behind the heat-index table everybody
// quotes. It is defined in degrees FAHRENHEIT and percent RH, so the
// conversions happen inside and the boundary stays SI.
//
// Two adjustments from the same source, both of which SensESP omitted and both
// of which the published table contains:
//
//   * dry and hot (RH < 13%, 80..112 F): subtract
//         ((13 - RH) / 4) * sqrt((17 - |T - 95|) / 17)
//   * humid and hot (RH > 85%, 80..87 F): add
//         ((RH - 85) / 10) * ((87 - T) / 5)
//
// Below about 80 F the regression is not valid at all; NOAA uses a simpler
// average there, and so does this, with the crossover NOAA specifies (the
// simple form is used when its own result is below 80 F).
inline float heat_index_k(float temp_k, float rh_ratio) {
  const float t = units::k_to_f(temp_k);
  float rh = rh_ratio * 100.0f;
  if (rh < 0.0f) rh = 0.0f;
  if (rh > 100.0f) rh = 100.0f;

  // The simple form first. NOAA's rule is to use the full regression only if
  // this average is 80 F or above, which keeps the heat index equal to the
  // temperature in conditions where "it feels hotter" is meaningless.
  const float simple =
      0.5f * (t + 61.0f + ((t - 68.0f) * 1.2f) + (rh * 0.094f));
  if (0.5f * (simple + t) < 80.0f) return units::f_to_k(simple);

  float hi = -42.379f + 2.04901523f * t + 10.14333127f * rh -
             0.22475541f * t * rh - 0.00683783f * t * t -
             0.05481717f * rh * rh + 0.00122874f * t * t * rh +
             0.00085282f * t * rh * rh - 0.00000199f * t * t * rh * rh;

  if (rh < 13.0f && t >= 80.0f && t <= 112.0f) {
    hi -= ((13.0f - rh) / 4.0f) *
          std::sqrt((17.0f - std::fabs(t - 95.0f)) / 17.0f);
  } else if (rh > 85.0f && t >= 80.0f && t <= 87.0f) {
    hi += ((rh - 85.0f) / 10.0f) * ((87.0f - t) / 5.0f);
  }
  return units::f_to_k(hi);
}

// How much hotter it feels than it is, in kelvin: heat index minus the
// temperature. SensESP's HeatIndexEffect. This is the number worth publishing
// as an alarm threshold — an engine room 8 K worse than ambient is a fact
// about the engine room, whereas the absolute heat index is mostly a fact
// about the weather.
inline float heat_index_effect_k(float temp_k, float rh_ratio) {
  return heat_index_k(temp_k, rh_ratio) - temp_k;
}

// ─────────────────────────────────────────────────────── air density
//
// Humid-air density in kg/m3, from the ideal gas law applied separately to the
// dry and vapour partial pressures (CIPM's simplified form):
//
//   rho = (Pd * Md + Pv * Mv) / (R * T)
//
// with Pv the vapour pressure and Pd = P - Pv. Moist air is LIGHTER than dry
// air at the same pressure, which surprises people — water is lighter than the
// nitrogen it displaces. It matters for a sailmaker's true-wind-power figure
// and for an engine's mass airflow, which is why it is here.
inline float air_density(float temp_k, float pressure_pa, float rh_ratio) {
  const float pv = vapour_pressure_pa(temp_k, rh_ratio);
  const float pd = pressure_pa - pv;
  return (pd * units::kMolarMassDryAir + pv * units::kMolarMassWaterVapour) /
         (units::kGasConstant * temp_k);
}

// ───────────────────────────────────────────────── resistive dividers
//
// The shape of nearly every legacy marine sender. A fixed resistor and the
// sender form a divider across a known supply, and the ADC reads the midpoint:
//
//        Vin ──[ R1 ]──┬──[ R2 ]── GND
//                      └── ADC (Vout)
//
// Three forms, because three different things are unknown depending on which
// leg the sender is:
//
//   * DividerR1 — sender in the TOP leg, R2 fixed and known. Recovers R1.
//   * DividerR2 — sender in the BOTTOM leg, R1 fixed and known. Recovers R2.
//     This is the one tank and temperature senders need: they are almost
//     always the low-side element, one terminal grounded to the hull.
//   * DividerScale — neither leg unknown; just undo the attenuation to get
//     back to the input voltage. This is a voltage-monitoring divider on a
//     12 V bus, not a sender at all.
//
// Every one of them can be handed a division by zero by a disconnected sensor
// (Vout == 0 or Vout == Vin), so every one returns an optional. SensESP
// returned inf and published it; an open-circuit tank sender then reads as an
// infinite level, which a plotter draws as a full tank. Nothing is the honest
// answer to a disconnected wire.

// Sender in the top leg: R1 = R2 * (Vin - Vout) / Vout.
// Vout of 0 means the sender is open (or the wire is off): no current, no
// midpoint, no resistance to compute.
inline std::optional<float> divider_r1(float v_out, float v_in, float r2) {
  if (!(v_out > 0.0f) || !(r2 > 0.0f)) return std::nullopt;
  return r2 * (v_in - v_out) / v_out;
}

// Sender in the bottom leg: R2 = R1 * Vout / (Vin - Vout).
// Vout == Vin means the sender is open: nothing pulls the midpoint down.
inline std::optional<float> divider_r2(float v_out, float v_in, float r1) {
  const float head = v_in - v_out;
  if (!(head > 0.0f) || !(r1 > 0.0f)) return std::nullopt;
  return r1 * v_out / head;
}

// Undo a known attenuation: Vin = Vout * (R1 + R2) / R2.
// Not an optional — R2 > 0 is a build-time property of a divider you soldered,
// not a runtime condition, and a zero is a wiring mistake rather than a
// disconnected sender.
inline float divider_scale(float v_out, float r1, float r2) {
  if (!(r2 > 0.0f)) return v_out;
  return v_out * (r1 + r2) / r2;
}

// ───────────────────────────────────────────────────────── frequency
//
// Counts per period to a rate. SensESP's Frequency, plus the multiplier that
// every real installation needs and that SensESP made you write a second
// transform for.
//
// A tachometer sender does not give one pulse per revolution: an alternator
// W terminal gives (pole pairs) per revolution — typically 6 — and an
// aftermarket pickup gives whatever the flywheel has teeth. So the useful
// quantity is
//
//   Hz = count / (period_s * pulses_per_unit)
//
// and `pulses_per_unit` of 6 turns an alternator's W terminal into engine
// revolutions per second, which is what Signal K's propulsion.*.revolutions
// wants. Leave it at 1 for a raw pulse rate (a flow meter's K-factor is the
// same idea in litres).
inline std::optional<float> frequency_hz(uint32_t count, float period_s,
                                         float pulses_per_unit = 1.0f) {
  if (!(period_s > 0.0f) || !(pulses_per_unit > 0.0f)) return std::nullopt;
  return static_cast<float>(count) / (period_s * pulses_per_unit);
}

// ──────────────────────────────────────────────────────── angle wrap
//
// Add an offset to an angle and wrap the result into a half-open interval of
// width 2pi starting at `min`.
//
// The two intervals that matter, and why both exist:
//
//   * [0, 2pi)   — Signal K's rule for a heading or a bearing. A compass reads
//                  350 degrees; it never reads -10.
//   * [-pi, pi)  — the rule for a RELATIVE angle: apparent wind, rudder, or a
//                  cross-track error, where the sign is the whole point and
//                  "5 degrees to port" must not become 355.
//
// SensESP's AngleCorrection did the first only, which is why every project
// wiring an apparent-wind vane ended up writing the second by hand.
inline float wrap_angle(float rad, float min = 0.0f) {
  const float two_pi = 2.0f * units::kPi;
  const float max = min + two_pi;
  if (!std::isfinite(rad)) return rad;
  // fmod once rather than a while loop: a sensor glitch handing us 1e6 radians
  // would spin a loop for a quarter of a million iterations on the flow task.
  float v = std::fmod(rad - min, two_pi);
  if (v < 0.0f) v += two_pi;
  v += min;
  // fmod's rounding can land exactly on the open end; fold it to the closed
  // one so the interval really is half-open.
  if (v >= max) v -= two_pi;
  return v;
}

// Offset then wrap: the calibration for a compass whose mount is not aligned
// with the bow, or a wind vane whose zero is not dead ahead.
inline float angle_offset(float rad, float offset_rad, float min = 0.0f) {
  return wrap_angle(rad + offset_rad, min);
}

// ─────────────────────────────────────────────────────── tank level
//
// Linear map from a sender reading to a Signal K ratio 0..1, clamped.
//
// `empty` and `full` are the sender readings at those two states, in whatever
// unit the sender speaks — ohms, volts, millimetres. They may be in either
// order: a resistive tank sender is very often 240 ohm empty and 33 ohm full
// (the US standard), so `full < empty` is the common case, not an error.
//
// The clamp is not optional. A sender reads slightly outside its calibration
// on a heeled boat every single tack, and a tank that reads 103% or -2% turns
// into an alarm, a bad average, or a fuel-consumption figure that goes
// backwards.
inline float tank_level(float reading, float empty, float full) {
  const float span = full - empty;
  if (span == 0.0f) return 0.0f;
  float r = (reading - empty) / span;
  if (r < 0.0f) r = 0.0f;
  if (r > 1.0f) r = 1.0f;
  return r;
}

// ───────────────────────────────────────────────────── battery state
//
// Open-circuit-voltage state of charge, for the two chemistries on boats.
//
// ── The caveat that makes this honest ────────────────────────────────────
//
// Voltage-based SoC is only meaningful AT REST: no charging, no significant
// load, and settled for long enough that surface charge has dissipated —
// 30 minutes for AGM, and LiFePO4 wants hours to be worth much. Under load a
// battery reads low and under charge it reads high, so the number this returns
// during an engine run is a fiction.
//
// For LiFePO4 the whole idea is marginal even at rest: the discharge curve is
// deliberately flat, and between roughly 20% and 90% the entire span is about
// 100 mV per cell. A 10 mV measurement error there is 10% of the battery.
// A shunt-based coulomb counter is the real answer and there is no arithmetic
// that substitutes for one.
//
// So: publish this as an estimate, alarm on the ends where the curve is steep
// and the reading is trustworthy, and do not budget a passage on the middle.
// It is here because "roughly how full is it" while the boat sits on a mooring
// is a genuinely useful thing a voltage divider can already answer, and
// because leaving it out means every project rewrites it worse.
//
// Curves are per 12 V nominal battery (multiply the input by 12/nominal for a
// 24 V bank before calling). Both are rest voltages at ~25 C.

// LiFePO4, 4 cells in series. Points from the flat part of the curve are
// deliberately sparse — interpolating finely through a plateau invents
// precision that is not in the measurement.
inline constexpr CurveSample kLifepo4Soc12v[] = {
    {10.00f, 0.00f},  // 2.50 V/cell — protection cutoff territory
    {12.00f, 0.05f},  // 3.00 V/cell — the knee; below this it falls off a cliff
    {12.80f, 0.10f}, {13.00f, 0.20f}, {13.10f, 0.40f},
    {13.20f, 0.60f},  // the plateau: 13.0..13.3 V is most of the battery
    {13.25f, 0.80f}, {13.40f, 0.90f}, {13.60f, 0.99f},
    {14.60f, 1.00f},  // 3.65 V/cell — fully charged, absorption voltage
};
inline constexpr std::size_t kLifepo4Soc12vCount =
    sizeof(kLifepo4Soc12v) / sizeof(kLifepo4Soc12v[0]);

// AGM lead-acid, 6 cells. A far more usable curve than LiFePO4's — the slope
// is roughly linear over the whole range, which is the one advantage lead has
// left. 50% is the practical floor: taking an AGM below it repeatedly halves
// its life, so treat 0.5 as "empty" for planning even though the curve runs on.
inline constexpr CurveSample kAgmSoc12v[] = {
    {11.60f, 0.00f}, {11.80f, 0.10f}, {11.95f, 0.20f}, {12.10f, 0.30f},
    {12.20f, 0.40f}, {12.30f, 0.50f}, {12.40f, 0.60f}, {12.50f, 0.70f},
    {12.60f, 0.80f}, {12.70f, 0.90f}, {12.80f, 1.00f},
};
inline constexpr std::size_t kAgmSoc12vCount =
    sizeof(kAgmSoc12v) / sizeof(kAgmSoc12v[0]);

enum class BatteryChemistry : uint8_t {
  kLifepo4,
  kAgm,
};

// Estimated state of charge, 0..1, from a RESTING terminal voltage.
// `nominal_v` scales a 24 V or 48 V bank onto the 12 V curves.
inline float battery_soc(float volts, BatteryChemistry chem,
                         float nominal_v = 12.0f) {
  const float v12 = (nominal_v > 0.0f) ? volts * (12.0f / nominal_v) : volts;
  const CurveSample* tbl =
      (chem == BatteryChemistry::kAgm) ? kAgmSoc12v : kLifepo4Soc12v;
  const std::size_t n =
      (chem == BatteryChemistry::kAgm) ? kAgmSoc12vCount : kLifepo4Soc12vCount;
  // curve_eval clamps outside the table, which is exactly right here: 15 V is
  // a battery on a charger, not a battery at 140%.
  return curve_eval_or(tbl, n, v12, 0.0f);
}

// ────────────────────────────────────────────────── thermistor presets
//
// Temperature curves for the senders actually fitted to marine engines, as
// resistance-to-kelvin tables for Curve. They exist because the alternative is
// every user measuring their own engine's sender with a heat gun, and because
// the two standards below cover most of the fleet.
//
// These are nominal manufacturer curves, not calibrations of YOUR sender: a
// 30-year-old sender with a corroded earth reads high, and the point of Curve
// being editable is that you can correct it. Use these as a starting table in
// the UI, then fix the two or three points you can verify against a known
// temperature.

// US standard coolant/oil temperature sender, 33..240 ohm (the same range as
// the tank senders, which is why the two get confused). Resistance FALLS with
// temperature — an NTC — so the table is entered ascending by resistance,
// which means descending by temperature.
inline constexpr CurveSample kThermistorUs240_33[] = {
    {33.0f, 393.15f},   // 120 C
    {50.0f, 373.15f},   // 100 C
    {75.0f, 358.15f},   // 85 C
    {110.0f, 343.15f},  // 70 C
    {160.0f, 328.15f},  // 55 C
    {240.0f, 313.15f},  // 40 C
};
inline constexpr std::size_t kThermistorUs240_33Count =
    sizeof(kThermistorUs240_33) / sizeof(kThermistorUs240_33[0]);

// European standard (VDO/Volvo Penta), 10..180 ohm.
inline constexpr CurveSample kThermistorVdo180_10[] = {
    {10.0f, 393.15f},   // 120 C
    {22.0f, 373.15f},   // 100 C
    {32.0f, 358.15f},   // 85 C
    {51.0f, 343.15f},   // 70 C
    {87.0f, 328.15f},   // 55 C
    {180.0f, 313.15f},  // 40 C
};
inline constexpr std::size_t kThermistorVdo180_10Count =
    sizeof(kThermistorVdo180_10) / sizeof(kThermistorVdo180_10[0]);

// A generic NTC by its datasheet parameters, for a thermistor you soldered on
// yourself rather than one that came with an engine. The beta equation:
//
//   1/T = 1/T0 + (1/B) * ln(R / R0)
//
// Beta is a two-point fit and drifts a few kelvin over a wide span; that is
// still better than a curve you have not measured, and it is exact at T0.
inline std::optional<float> ntc_beta_k(float resistance_ohm, float r0_ohm,
                                       float t0_k, float beta) {
  if (!(resistance_ohm > 0.0f) || !(r0_ohm > 0.0f) || !(t0_k > 0.0f) ||
      !(beta > 0.0f)) {
    return std::nullopt;
  }
  const float inv = 1.0f / t0_k + std::log(resistance_ohm / r0_ohm) / beta;
  if (!(inv > 0.0f)) return std::nullopt;
  return 1.0f / inv;
}

}  // namespace espos::formulas
