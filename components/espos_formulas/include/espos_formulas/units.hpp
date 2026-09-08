// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::units — the conversions between what a sensor reads and what Signal K
// stores.
//
// Signal K is SI, without exception: kelvin, metres per second, radians,
// pascals, cubic metres per second, ratio 0..1. Getting that wrong is the
// single most common mistake in marine firmware, and it is a quiet one — a
// depth published in feet looks like a plausible depth, and the plotter draws
// a shoal where there is none. So every conversion a boat needs lives here,
// once, as a constexpr function with the defining constant spelled out and
// cited, rather than as a magic number retyped in each project.
//
// Everything is `constexpr` and takes/returns `float`: on an ESP32 without an
// FPU-wide double path, float is what the graph carries, and the conversions
// fold away entirely when the argument is a literal.
//
// Direction naming is `from_to`: `kn_to_ms(6.0f)` reads as "six knots, in
// metres per second". Where a round trip is likely, both directions exist;
// where it is not (nobody publishes fathoms), only the SI-ward one does.
//
// Pure arithmetic — no IDF, no allocation, no exceptions. The host test is the
// real test, and it checks every function against its SI definition rather
// than against another espOS function.
#pragma once

#include <cmath>

namespace espos::units {

// ───────────────────────────────────────────────────────────── constants
//
// Each of these is a definition, not a measurement, so they are exact and a
// test may compare against the same literal without being circular — the test
// spells the SI definition, this spells the same definition, and a typo in
// either shows up as a mismatch.

// The Celsius scale is defined by an offset from kelvin (BIPM SI brochure).
inline constexpr float kZeroCelsiusK = 273.15f;

// One international nautical mile is exactly 1852 m (BIPM, 1929 First
// International Extraordinary Hydrographic Conference).
inline constexpr float kNauticalMileM = 1852.0f;

// A knot is one nautical mile per hour, so exactly 1852/3600 m/s.
inline constexpr float kKnotMs = kNauticalMileM / 3600.0f;

// The international foot is exactly 0.3048 m (1959 international yard and
// pound agreement). NOT the US survey foot: they differ by 2 ppm, which is
// 6 mm over a nautical mile — irrelevant for a depth sounder and wrong for a
// survey, and we are not a survey.
inline constexpr float kFootM = 0.3048f;

// A fathom is six feet by definition.
inline constexpr float kFathomM = 6.0f * kFootM;

// One bar is exactly 100000 Pa (SI-accepted, not SI).
inline constexpr float kBarPa = 100000.0f;

// One pound per square inch: the avoirdupois pound-force (4.4482216152605 N,
// exact by definition of the pound and standard gravity) over a square inch
// (0.0254 m squared). 6894.757293168361 Pa; float keeps ~7 digits of it.
inline constexpr float kPsiPa = 6894.757293168361f;

// The US liquid gallon is exactly 231 cubic inches = 3.785411784 L.
// An Imperial gallon is 4.54609 L — a 21% difference, which is why the two
// have separate names here instead of one "gallon".
inline constexpr float kUsGallonL = 3.785411784f;
inline constexpr float kImperialGallonL = 4.54609f;

// Pi to float precision, spelled out rather than taken from a macro that is
// not guaranteed to exist (M_PI is POSIX, not ISO C++).
inline constexpr float kPi = 3.14159265358979323846f;

// The universal gas constant, and the molar masses AirDensity and DewPoint
// need. CODATA 2018: R is exact since the 2019 SI redefinition.
inline constexpr float kGasConstant = 8.31446261815324f;   // J/(mol*K)
inline constexpr float kMolarMassDryAir = 0.0289652f;      // kg/mol
inline constexpr float kMolarMassWaterVapour = 0.018016f;  // kg/mol

// ───────────────────────────────────────────────────── temperature
//
// Signal K stores kelvin. Every marine temperature sensor reads Celsius or
// Fahrenheit, and every user types one of those, so both directions exist.

constexpr float c_to_k(float c) { return c + kZeroCelsiusK; }
constexpr float k_to_c(float k) { return k - kZeroCelsiusK; }
constexpr float f_to_c(float f) { return (f - 32.0f) * (5.0f / 9.0f); }
constexpr float c_to_f(float c) { return c * (9.0f / 5.0f) + 32.0f; }
constexpr float f_to_k(float f) { return c_to_k(f_to_c(f)); }
constexpr float k_to_f(float k) { return c_to_f(k_to_c(k)); }

// ─────────────────────────────────────────────────────────── speed

constexpr float kn_to_ms(float kn) { return kn * kKnotMs; }
constexpr float ms_to_kn(float ms) { return ms / kKnotMs; }
constexpr float kmh_to_ms(float kmh) { return kmh / 3.6f; }
constexpr float ms_to_kmh(float ms) { return ms * 3.6f; }
constexpr float mph_to_ms(float mph) { return mph * (1609.344f / 3600.0f); }

// ─────────────────────────────────────────────────────────── angle
//
// Signal K stores radians, 0..2pi, and every compass, wind vane and rudder
// sender speaks degrees. This pair is the most-used conversion in the file.

constexpr float deg_to_rad(float deg) { return deg * (kPi / 180.0f); }
constexpr float rad_to_deg(float rad) { return rad * (180.0f / kPi); }

// ──────────────────────────────────────────────────────── pressure

constexpr float bar_to_pa(float bar) { return bar * kBarPa; }
constexpr float pa_to_bar(float pa) { return pa / kBarPa; }
constexpr float psi_to_pa(float psi) { return psi * kPsiPa; }
constexpr float pa_to_psi(float pa) { return pa / kPsiPa; }
// A hectopascal IS a millibar; barometers are quoted in both and the number is
// identical, which is worth stating because it looks like it should not be.
constexpr float hpa_to_pa(float hpa) { return hpa * 100.0f; }
constexpr float pa_to_hpa(float pa) { return pa / 100.0f; }
constexpr float mbar_to_pa(float mbar) { return mbar * 100.0f; }
constexpr float inhg_to_pa(float inhg) { return inhg * 3386.389f; }

// ──────────────────────────────────────────────────────────── flow
//
// Signal K stores volumetric flow in cubic metres per second. A fuel-flow
// sender reads litres or gallons per hour, so the numbers involved are tiny —
// 10 L/h is 2.8e-6 m3/s — which is exactly why nobody gets this right by hand.

constexpr float lph_to_m3s(float lph) { return lph / (1000.0f * 3600.0f); }
constexpr float m3s_to_lph(float m3s) { return m3s * (1000.0f * 3600.0f); }
constexpr float gph_to_m3s(float gph) {
  return gph * kUsGallonL / (1000.0f * 3600.0f);
}
constexpr float m3s_to_gph(float m3s) {
  return m3s * (1000.0f * 3600.0f) / kUsGallonL;
}
constexpr float lpm_to_m3s(float lpm) { return lpm / (1000.0f * 60.0f); }

// ────────────────────────────────────────────────────────── volume

constexpr float l_to_m3(float l) { return l / 1000.0f; }
constexpr float m3_to_l(float m3) { return m3 * 1000.0f; }
constexpr float gal_to_m3(float gal) { return gal * kUsGallonL / 1000.0f; }
constexpr float m3_to_gal(float m3) { return m3 * 1000.0f / kUsGallonL; }
constexpr float imp_gal_to_m3(float gal) {
  return gal * kImperialGallonL / 1000.0f;
}

// ──────────────────────────────────────────────────────── distance

constexpr float ft_to_m(float ft) { return ft * kFootM; }
constexpr float m_to_ft(float m) { return m / kFootM; }
constexpr float fathom_to_m(float f) { return f * kFathomM; }
constexpr float m_to_fathom(float m) { return m / kFathomM; }
constexpr float nm_to_m(float nm) { return nm * kNauticalMileM; }
constexpr float m_to_nm(float m) { return m / kNauticalMileM; }
constexpr float in_to_m(float in) { return in * 0.0254f; }

// ─────────────────────────────────────────────────────────── ratio
//
// Signal K's "ratio" is 0..1, not 0..100. A tank level published as 87 rather
// than 0.87 reads as 8700% on the plotter, which is the kind of thing that
// looks like a server bug for a week.

constexpr float percent_to_ratio(float pct) { return pct / 100.0f; }
constexpr float ratio_to_percent(float r) { return r * 100.0f; }

// ──────────────────────────────────────────────────────── rotation
//
// Signal K stores revolutions per SECOND, and every tachometer and every user
// thinks in RPM. This is the conversion most often missed, because a value of
// 3000 where 50 was meant still looks like an engine speed.

constexpr float rpm_to_hz(float rpm) { return rpm / 60.0f; }
constexpr float hz_to_rpm(float hz) { return hz * 60.0f; }

// ───────────────────────────────────────────────────────── charge
//
// Amp-hours are coulombs; Signal K's electrical capacity is coulombs too.

constexpr float ah_to_c(float ah) { return ah * 3600.0f; }
constexpr float c_to_ah(float c) { return c / 3600.0f; }

}  // namespace espos::units
