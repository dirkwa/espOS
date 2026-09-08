// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// Unit conversions against their SI DEFINITIONS.
//
// The rule this file follows: never check a conversion against another espOS
// function, and never against a number produced by running the code. Each
// expectation is either the defining constant spelled out longhand (1852 m to
// the nautical mile, 0.3048 m to the foot) or a value from an independent
// source. A test that says ms_to_kn(kn_to_ms(x)) == x passes just as happily
// with the wrong constant in both.

#include <cmath>

#include "unity.h"

#include "espos_formulas/units.hpp"

using namespace espos::units;

// Relative tolerance for a float conversion. float carries ~7 significant
// digits; 1e-6 relative is the honest limit and catches a wrong constant in
// the fourth digit, which is what a typo looks like.
static void assert_close(double expected, double actual, double rel = 1e-6)
{
    const double tol = std::fabs(expected) * rel;
    TEST_ASSERT_DOUBLE_WITHIN(tol > 1e-9 ? tol : 1e-9, expected, actual);
}

// ─────────────────────────────────────────────────────────── temperature

TEST_CASE("c_to_k: the Celsius offset is exactly 273.15", "[units]")
{
    // BIPM SI brochure: 0 C = 273.15 K by definition.
    assert_close(273.15, c_to_k(0.0f));
    assert_close(373.15, c_to_k(100.0f));   // boiling point at 1 atm
    assert_close(255.372222, c_to_k(-17.777778f), 1e-5);
}

TEST_CASE("k_to_c inverts c_to_k at the definition points", "[units]")
{
    assert_close(0.0, k_to_c(273.15f), 1e-5);
    assert_close(100.0, k_to_c(373.15f), 1e-5);
    // Absolute zero in Celsius, the other end of the defined scale.
    assert_close(-273.15, k_to_c(0.0f), 1e-5);
}

TEST_CASE("Fahrenheit against the two defining points", "[units]")
{
    // The Fahrenheit scale is defined by 32 F = 0 C and 212 F = 100 C.
    assert_close(0.0, f_to_c(32.0f), 1e-5);
    assert_close(100.0, f_to_c(212.0f), 1e-5);
    assert_close(32.0, c_to_f(0.0f), 1e-5);
    assert_close(212.0, c_to_f(100.0f), 1e-5);
    // -40 is the scales' crossing point: the one value that is its own
    // conversion, and a check that the slope AND the offset are both right.
    assert_close(-40.0, f_to_c(-40.0f), 1e-5);
    assert_close(-40.0, c_to_f(-40.0f), 1e-5);
    assert_close(273.15, f_to_k(32.0f), 1e-5);
    assert_close(32.0, k_to_f(273.15f), 1e-5);
}

// ───────────────────────────────────────────────────────────────── speed

TEST_CASE("kn_to_ms: one nautical mile per hour, 1852/3600", "[units]")
{
    // BIPM: the international nautical mile is exactly 1852 m; a knot is one
    // of them per hour. 1852/3600 = 0.5144444... m/s.
    assert_close(1852.0 / 3600.0, kn_to_ms(1.0f));
    assert_close(5.0 * 1852.0 / 3600.0, kn_to_ms(5.0f));
    // A hull speed a boat actually sails at, and its m/s value.
    assert_close(6.0 * 0.514444444, kn_to_ms(6.0f), 1e-6);
}

TEST_CASE("ms_to_kn: 1 m/s is 3600/1852 knots", "[units]")
{
    assert_close(3600.0 / 1852.0, ms_to_kn(1.0f));
    assert_close(10.0 * 3600.0 / 1852.0, ms_to_kn(10.0f));
}

TEST_CASE("km/h and mph against their definitions", "[units]")
{
    // 1 km/h = 1000 m / 3600 s.
    assert_close(1000.0 / 3600.0, kmh_to_ms(1.0f));
    assert_close(3.6, ms_to_kmh(1.0f));
    // A statute mile is exactly 1609.344 m (1959 international agreement).
    assert_close(1609.344 / 3600.0, mph_to_ms(1.0f));
}

// ───────────────────────────────────────────────────────────────── angle

TEST_CASE("deg_to_rad at the quadrant boundaries", "[units]")
{
    const double pi = 3.14159265358979323846;
    assert_close(0.0, deg_to_rad(0.0f), 1e-6);
    assert_close(pi / 2.0, deg_to_rad(90.0f), 1e-6);
    assert_close(pi, deg_to_rad(180.0f), 1e-6);
    assert_close(2.0 * pi, deg_to_rad(360.0f), 1e-6);
    // One degree, the resolution of a fluxgate compass.
    assert_close(pi / 180.0, deg_to_rad(1.0f), 1e-6);
}

TEST_CASE("rad_to_deg at the quadrant boundaries", "[units]")
{
    const double pi = 3.14159265358979323846;
    assert_close(90.0, rad_to_deg(static_cast<float>(pi / 2.0)), 1e-5);
    assert_close(180.0, rad_to_deg(static_cast<float>(pi)), 1e-5);
    assert_close(57.295779513, rad_to_deg(1.0f), 1e-6);  // one radian
}

// ────────────────────────────────────────────────────────────── pressure

TEST_CASE("bar_to_pa: one bar is exactly 100000 Pa", "[units]")
{
    assert_close(100000.0, bar_to_pa(1.0f));
    assert_close(250000.0, bar_to_pa(2.5f));
    assert_close(1.0, pa_to_bar(100000.0f));
}

TEST_CASE("psi_to_pa against the pound-force definition", "[units]")
{
    // 1 lbf = 4.4482216152605 N exactly (pound = 0.45359237 kg, g = 9.80665);
    // 1 in^2 = 0.00064516 m^2 exactly. The quotient is 6894.757293168 Pa.
    assert_close(6894.757293168, psi_to_pa(1.0f), 1e-6);
    // 30 psi, a typical engine oil pressure.
    assert_close(30.0 * 6894.757293168, psi_to_pa(30.0f), 1e-6);
    assert_close(1.0, pa_to_psi(6894.757293168f), 1e-5);
}

TEST_CASE("hPa and mbar are the same number, both 100 Pa", "[units]")
{
    assert_close(100.0, hpa_to_pa(1.0f));
    assert_close(100.0, mbar_to_pa(1.0f));
    // Standard sea-level pressure, the number on every barometer.
    assert_close(101325.0, hpa_to_pa(1013.25f), 1e-6);
    assert_close(1013.25, pa_to_hpa(101325.0f), 1e-6);
}

TEST_CASE("inHg: standard atmosphere is 29.9213 inHg", "[units]")
{
    // 101325 Pa / 3386.389 Pa per inHg = 29.9213 inHg, the US barometer unit.
    assert_close(101325.0, inhg_to_pa(29.92126f), 1e-4);
}

// ────────────────────────────────────────────────────────────────── flow

TEST_CASE("lph_to_m3s: litres per hour to cubic metres per second", "[units]")
{
    // 1 L/h = 0.001 m3 / 3600 s = 2.7777...e-7 m3/s.
    assert_close(1.0 / 3600000.0, lph_to_m3s(1.0f));
    // A small diesel burning 10 L/h.
    assert_close(10.0 / 3600000.0, lph_to_m3s(10.0f));
    assert_close(10.0, m3s_to_lph(static_cast<float>(10.0 / 3600000.0)), 1e-5);
}

TEST_CASE("gph_to_m3s uses the US gallon of 3.785411784 L", "[units]")
{
    assert_close(3.785411784 / 3600000.0, gph_to_m3s(1.0f), 1e-6);
    assert_close(1.0, m3s_to_gph(static_cast<float>(3.785411784 / 3600000.0)),
                 1e-5);
}

TEST_CASE("lpm_to_m3s", "[units]")
{
    assert_close(1.0 / 60000.0, lpm_to_m3s(1.0f));
}

// ──────────────────────────────────────────────────────────────── volume

TEST_CASE("litres and cubic metres", "[units]")
{
    assert_close(0.001, l_to_m3(1.0f));
    assert_close(0.2, l_to_m3(200.0f), 1e-6);   // a 200 L water tank
    assert_close(1000.0, m3_to_l(1.0f));
}

TEST_CASE("US gallon is 231 cubic inches = 3.785411784 L", "[units]")
{
    // 231 in^3 * (0.0254 m)^3 = 0.003785411784 m3, exact by definition.
    assert_close(231.0 * 0.0254 * 0.0254 * 0.0254, gal_to_m3(1.0f), 1e-6);
    assert_close(0.003785411784, gal_to_m3(1.0f), 1e-6);
    assert_close(1.0, m3_to_gal(0.003785411784f), 1e-5);
}

TEST_CASE("Imperial gallon is 4.54609 L, NOT the US one", "[units]")
{
    assert_close(0.00454609, imp_gal_to_m3(1.0f), 1e-6);
    // The two differ by about 20%: a 50-gallon tank is 189 L or 227 L
    // depending on which side of the Atlantic printed the label.
    TEST_ASSERT_TRUE(imp_gal_to_m3(1.0f) > gal_to_m3(1.0f) * 1.19f);
}

// ──────────────────────────────────────────────────────────── distance

TEST_CASE("ft_to_m: the international foot is exactly 0.3048 m", "[units]")
{
    assert_close(0.3048, ft_to_m(1.0f));
    assert_close(3.048, ft_to_m(10.0f), 1e-6);
    assert_close(1.0, m_to_ft(0.3048f), 1e-5);
}

TEST_CASE("fathom is six feet = 1.8288 m", "[units]")
{
    assert_close(6.0 * 0.3048, fathom_to_m(1.0f), 1e-6);
    assert_close(1.8288, fathom_to_m(1.0f), 1e-6);
    // Five fathoms, the classic sounding.
    assert_close(9.144, fathom_to_m(5.0f), 1e-6);
    assert_close(5.0, m_to_fathom(9.144f), 1e-5);
}

TEST_CASE("nautical mile is exactly 1852 m", "[units]")
{
    assert_close(1852.0, nm_to_m(1.0f));
    assert_close(1.0, m_to_nm(1852.0f), 1e-6);
    // A cable is a tenth of a nautical mile.
    assert_close(185.2, nm_to_m(0.1f), 1e-5);
}

TEST_CASE("inch is exactly 0.0254 m", "[units]")
{
    assert_close(0.0254, in_to_m(1.0f));
}

// ─────────────────────────────────────────────────────────────── ratio

TEST_CASE("percent_to_ratio: Signal K wants 0..1, not 0..100", "[units]")
{
    assert_close(0.0, percent_to_ratio(0.0f), 1e-9);
    assert_close(0.5, percent_to_ratio(50.0f), 1e-6);
    assert_close(1.0, percent_to_ratio(100.0f), 1e-6);
    assert_close(0.87, percent_to_ratio(87.0f), 1e-6);
    assert_close(87.0, ratio_to_percent(0.87f), 1e-5);
}

// ──────────────────────────────────────────────────────────── rotation

TEST_CASE("rpm_to_hz: Signal K stores revolutions per SECOND", "[units]")
{
    // The most-missed conversion in marine firmware: 3000 RPM is 50 Hz, and
    // publishing 3000 into propulsion.*.revolutions is off by sixty.
    assert_close(50.0, rpm_to_hz(3000.0f), 1e-6);
    assert_close(1.0, rpm_to_hz(60.0f), 1e-6);
    // Idle, around 800 RPM.
    assert_close(13.333333, rpm_to_hz(800.0f), 1e-6);
    assert_close(3000.0, hz_to_rpm(50.0f), 1e-6);
}

// ─────────────────────────────────────────────────────────────── charge

TEST_CASE("amp-hours to coulombs", "[units]")
{
    // 1 Ah = 1 A * 3600 s = 3600 C.
    assert_close(3600.0, ah_to_c(1.0f), 1e-6);
    // A 200 Ah house bank.
    assert_close(720000.0, ah_to_c(200.0f), 1e-6);
    assert_close(200.0, c_to_ah(720000.0f), 1e-5);
}

// ──────────────────────────────────────────────────── constants themselves

TEST_CASE("the defining constants are what they are defined to be", "[units]")
{
    // Spelled out here as a second, independent statement of each definition:
    // if a constant is edited, this fails even if every conversion above was
    // edited to match.
    TEST_ASSERT_EQUAL_FLOAT(273.15f, kZeroCelsiusK);
    TEST_ASSERT_EQUAL_FLOAT(1852.0f, kNauticalMileM);
    TEST_ASSERT_EQUAL_FLOAT(0.3048f, kFootM);
    TEST_ASSERT_EQUAL_FLOAT(1.8288f, kFathomM);
    TEST_ASSERT_EQUAL_FLOAT(100000.0f, kBarPa);
    assert_close(6894.757293168, kPsiPa, 1e-6);
    assert_close(3.785411784, kUsGallonL, 1e-6);
    assert_close(4.54609, kImperialGallonL, 1e-6);
    assert_close(3.14159265358979, kPi, 1e-7);
    // CODATA / SI 2019: R is exact.
    assert_close(8.31446261815324, kGasConstant, 1e-7);
}

TEST_CASE("everything is constexpr: folded at compile time", "[units]")
{
    // If any of these stopped being constexpr the file would not compile,
    // which is the assertion. A conversion in a chain must cost nothing when
    // its argument is a literal.
    static_assert(c_to_k(0.0f) == 273.15f);
    static_assert(kn_to_ms(0.0f) == 0.0f);
    static_assert(percent_to_ratio(100.0f) == 1.0f);
    static_assert(ft_to_m(1.0f) == 0.3048f);
    static_assert(nm_to_m(1.0f) == 1852.0f);
    TEST_PASS();
}
