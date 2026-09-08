// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// The marine formulas against PUBLISHED reference values.
//
// Where each expectation comes from is named in the test. The ones worth
// pointing at:
//
//   * saturation vapour pressure at 20 C is 2.339 kPa in every meteorology
//     textbook and every steam table. Arden Buck must reproduce it, and it
//     does to four figures -- which validates the coefficients independently
//     of the dew-point algebra built on them.
//   * air density at ISA sea level (15 C, 101325 Pa, dry) is 1.2250 kg/m3 BY
//     DEFINITION of the International Standard Atmosphere. Reproducing that
//     number from the gas law is a real check on R and the molar masses.
//   * the heat-index values are cells of NOAA's own published chart, read off
//     the table rather than computed here.
//   * the divider results are the algebra done by hand, on ratios chosen so
//     the answers are round numbers a reader can verify in their head.

#include <cmath>

#include "unity.h"

#include "espos_formulas/marine.hpp"
#include "espos_formulas/units.hpp"

using namespace espos::formulas;
// espos::units is a SIBLING namespace of espos::formulas, not nested inside
// it, so the using-directive above does not reach it. The headers spell
// units:: fully; the tests do too, via this.
namespace units = espos::units;

static void assert_close(double expected, double actual, double abs)
{
    TEST_ASSERT_DOUBLE_WITHIN(abs, expected, actual);
}

// ─────────────────────────────────────────────────── saturation pressure

TEST_CASE("saturation vapour pressure against the steam table", "[buck]")
{
    // Textbook values for es over water. These are independent of espOS: any
    // meteorology reference gives 0.6112 kPa at 0 C and 2.339 kPa at 20 C.
    assert_close(611.21, saturation_vapour_pressure_pa(units::c_to_k(0.0f)),
                 0.5);
    assert_close(2338.8, saturation_vapour_pressure_pa(units::c_to_k(20.0f)),
                 2.0);
    assert_close(3168.7, saturation_vapour_pressure_pa(units::c_to_k(25.0f)),
                 3.0);
    // At 100 C it must land on standard atmospheric pressure -- that is what
    // "boiling point" means, and it is the strongest single check on the
    // coefficients because it is 166 times the value at 0 C.
    assert_close(101325.0, saturation_vapour_pressure_pa(units::c_to_k(100.0f)),
                 150.0);
}

TEST_CASE("vapour pressure is saturation times RH, and clamps", "[buck]")
{
    const float t = units::c_to_k(20.0f);
    assert_close(0.5 * 2338.8, vapour_pressure_pa(t, 0.5f), 2.0);
    assert_close(0.0, vapour_pressure_pa(t, 0.0f), 1e-6);
    // Above saturation is not a thing: a sensor reading 1.05 is saturated.
    assert_close(saturation_vapour_pressure_pa(t), vapour_pressure_pa(t, 1.5f),
                 1e-3);
}

// ──────────────────────────────────────────────────────────── dew point

TEST_CASE("dew point: Arden Buck at everyday cabin conditions", "[dewpoint]")
{
    // 20 C / 50% RH -> 9.25 C. This is the canonical worked example of the
    // Arden Buck equation and appears in its published form.
    auto dp = dew_point_k(units::c_to_k(20.0f), 0.5f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(9.2506, units::k_to_c(*dp), 0.01);

    // 25 C / 60% -> 16.63 C.
    dp = dew_point_k(units::c_to_k(25.0f), 0.6f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(16.6327, units::k_to_c(*dp), 0.01);

    // 30 C / 80% -> 26.00 C: the tropics, and the reason a boat sweats.
    dp = dew_point_k(units::c_to_k(30.0f), 0.8f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(25.9965, units::k_to_c(*dp), 0.01);
}

TEST_CASE("dew point: below freezing, where Magnus drifts", "[dewpoint]")
{
    // The case that motivated using Buck over the Magnus form SensESP had:
    // 0 C / 60% -> -6.85 C, and -10 C / 70% -> -14.48 C. Condensation on the
    // inside of a coachroof at these temperatures becomes ice, so a few tenths
    // of a kelvin is the difference between a damp bunk and a frozen one.
    auto dp = dew_point_k(units::c_to_k(0.0f), 0.6f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(-6.8453, units::k_to_c(*dp), 0.01);

    dp = dew_point_k(units::c_to_k(-10.0f), 0.7f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(-14.4753, units::k_to_c(*dp), 0.01);
}

TEST_CASE("dew point: at saturation it equals the temperature", "[dewpoint]")
{
    // A physical identity, not a fitted value: 100% RH means the air is
    // already at its dew point. Buck's inversion is a fit rather than an exact
    // algebraic inverse, so it reproduces the identity to a third of a kelvin
    // at the top of the range and to millikelvins around freezing. That
    // residual is a property of the published equation, not of this code --
    // asserting it tightly would be asserting a coincidence.
    for (float tc : { -5.0f, 0.0f, 15.0f, 20.0f, 35.0f }) {
        auto dp = dew_point_k(units::c_to_k(tc), 1.0f);
        TEST_ASSERT_TRUE(dp.has_value());
        assert_close(tc, units::k_to_c(*dp), 0.4);
    }
}

TEST_CASE("dew point: zero humidity has no answer, not -inf", "[dewpoint]")
{
    // SensESP returned -inf here and published it. ln(0) is not a dew point.
    auto dp = dew_point_k(units::c_to_k(20.0f), 0.0f);
    TEST_ASSERT_FALSE(dp.has_value());
    dp = dew_point_k(units::c_to_k(20.0f), -0.1f);
    TEST_ASSERT_FALSE(dp.has_value());
}

TEST_CASE("dew point: an over-range humidity clamps rather than fails",
          "[dewpoint]")
{
    // A cheap sensor in fog reads 1.02. That is saturated, not broken.
    auto dp = dew_point_k(units::c_to_k(18.0f), 1.05f);
    TEST_ASSERT_TRUE(dp.has_value());
    assert_close(18.0, units::k_to_c(*dp), 0.2);
}

TEST_CASE("dew point is never above the temperature", "[dewpoint]")
{
    // A physical invariant swept across the range a boat sees. A dew point
    // above the air temperature would mean condensation out of nowhere.
    for (int tc = -20; tc <= 45; tc += 5) {
        for (int rh = 5; rh <= 100; rh += 5) {
            auto dp = dew_point_k(units::c_to_k(static_cast<float>(tc)),
                                  static_cast<float>(rh) / 100.0f);
            TEST_ASSERT_TRUE(dp.has_value());
            TEST_ASSERT_TRUE(units::k_to_c(*dp) <=
                             static_cast<float>(tc) + 0.4f);
        }
    }
}

// ─────────────────────────────────────────────────────────── heat index

TEST_CASE("heat index against cells of the NOAA chart", "[heatindex]")
{
    // NOAA's published heat-index table, read off the chart. The regression
    // reproduces each cell to within the rounding the chart itself uses.
    struct Case {
        float tf;
        float rh_pct;
        float expect_f;
    };
    const Case cases[] = {
        { 90.0f, 40.0f, 91.0f },    // chart: 91
        { 100.0f, 40.0f, 109.0f },  // chart: 109
        { 90.0f, 70.0f, 106.0f },   // chart: 106
        { 110.0f, 40.0f, 136.0f },  // chart: 136
        { 86.0f, 90.0f, 105.0f },   // chart: 105
        { 94.0f, 55.0f, 106.0f },   // chart: 106
    };
    for (const Case &c : cases) {
        const float hi_k =
            heat_index_k(units::f_to_k(c.tf), c.rh_pct / 100.0f);
        assert_close(c.expect_f, units::k_to_f(hi_k), 1.0);
    }
}

TEST_CASE("heat index: the dry adjustment below 13% RH", "[heatindex]")
{
    // NOAA's first adjustment, which SensESP omitted. At 96 F / 10% the
    // unadjusted regression gives about 91.7 F; the adjustment subtracts
    // roughly 1.4 F for a chart value of 90 F.
    const float hi = units::k_to_f(heat_index_k(units::f_to_k(96.0f), 0.10f));
    assert_close(90.0f, hi, 1.0);
    // 100 F / 10% -> 94 on the chart.
    const float hi2 = units::k_to_f(heat_index_k(units::f_to_k(100.0f), 0.10f));
    assert_close(94.0f, hi2, 1.0);
}

TEST_CASE("heat index: the humid adjustment above 85% RH", "[heatindex]")
{
    // NOAA's second adjustment, also missing from SensESP. 84 F / 100% is 103
    // on the chart; without the adjustment the regression is about a degree
    // low.
    const float hi = units::k_to_f(heat_index_k(units::f_to_k(84.0f), 1.0f));
    assert_close(103.0f, hi, 1.5);
}

TEST_CASE("heat index below 80 F falls back to the simple form", "[heatindex]")
{
    // NOAA uses an average rather than the regression at mild temperatures,
    // where "it feels hotter" is not meaningful. The result must stay close to
    // the temperature rather than diverging.
    const float hi = units::k_to_f(heat_index_k(units::f_to_k(70.0f), 0.5f));
    TEST_ASSERT_TRUE(std::fabs(hi - 70.0f) < 5.0f);
    const float hi2 = units::k_to_f(heat_index_k(units::f_to_k(60.0f), 0.3f));
    TEST_ASSERT_TRUE(std::fabs(hi2 - 60.0f) < 8.0f);
}

TEST_CASE("heat index effect is the difference, and is positive when hot",
          "[heatindex]")
{
    const float t = units::f_to_k(95.0f);
    const float eff = heat_index_effect_k(t, 0.7f);
    const float hi = heat_index_k(t, 0.7f);
    assert_close(hi - t, eff, 1e-4);
    // At 95 F and 70% it feels far hotter: the chart says 126 F, an effect of
    // 31 F = 17 K.
    TEST_ASSERT_TRUE(eff > 10.0f);
}

// ──────────────────────────────────────────────────────────── air density

TEST_CASE("air density reproduces the ISA sea-level value", "[density]")
{
    // 1.2250 kg/m3 at 15 C and 101325 Pa dry is the DEFINITION of ISA sea
    // level. Getting it from the gas law validates R, Md and the arithmetic
    // together, against a number nothing in espOS produced.
    assert_close(1.2250, air_density(units::c_to_k(15.0f), 101325.0f, 0.0f),
                 0.001);
}

TEST_CASE("air density of dry air at 0 C", "[density]")
{
    // Textbook: 1.2922 kg/m3 at 0 C, 101325 Pa.
    assert_close(1.2922, air_density(units::c_to_k(0.0f), 101325.0f, 0.0f),
                 0.001);
}

TEST_CASE("humid air is LIGHTER than dry air at the same pressure",
          "[density]")
{
    // The counter-intuitive one: water vapour (18 g/mol) displaces nitrogen
    // and oxygen (29 g/mol). At 20 C saturated the difference is about 1%.
    const float dry = air_density(units::c_to_k(20.0f), 101325.0f, 0.0f);
    const float wet = air_density(units::c_to_k(20.0f), 101325.0f, 1.0f);
    TEST_ASSERT_TRUE(wet < dry);
    assert_close(1.2041, dry, 0.001);
    assert_close(1.1936, wet, 0.001);
}

// ──────────────────────────────────────────────────────── dividers

TEST_CASE("divider_r2: hand-calculated cases", "[divider]")
{
    // Half the supply across a 1k top resistor means the bottom leg is also
    // 1k. No arithmetic required to check this one.
    auto r = divider_r2(1.65f, 3.3f, 1000.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(1000.0, *r, 0.5);

    // R2 = R1 * Vout / (Vin - Vout) = 1000 * 0.5 / 2.8 = 178.5714 ohm.
    r = divider_r2(0.5f, 3.3f, 1000.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(178.5714, *r, 0.01);

    // 470 * 3.0 / 0.3 = 4700 ohm: a factor of ten, easy to verify by eye.
    r = divider_r2(3.0f, 3.3f, 470.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(4700.0, *r, 1.0);

    // 240 * 1.0 / 4.0 = 60 ohm, on a 5 V rail.
    r = divider_r2(1.0f, 5.0f, 240.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(60.0, *r, 0.01);
}

TEST_CASE("divider_r2: an open sender is nothing, not infinity", "[divider]")
{
    // Vout == Vin means nothing is pulling the midpoint down: the sender is
    // disconnected. SensESP produced inf here, which downstream reads as a
    // full tank. This must produce no value at all.
    auto r = divider_r2(3.3f, 3.3f, 1000.0f);
    TEST_ASSERT_FALSE(r.has_value());
    // Above the supply -- an ADC reading noise at the rail -- is the same
    // situation and must not produce a negative resistance.
    r = divider_r2(3.4f, 3.3f, 1000.0f);
    TEST_ASSERT_FALSE(r.has_value());
    // A zero fixed resistor is a wiring error, not a measurement.
    r = divider_r2(1.0f, 3.3f, 0.0f);
    TEST_ASSERT_FALSE(r.has_value());
}

TEST_CASE("divider_r1: hand-calculated cases", "[divider]")
{
    // Symmetric case again: half the supply, 1k bottom, so 1k top.
    auto r = divider_r1(1.65f, 3.3f, 1000.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(1000.0, *r, 0.5);

    // R1 = R2 * (Vin - Vout) / Vout = 1000 * 2.8 / 0.5 = 5600 ohm.
    r = divider_r1(0.5f, 3.3f, 1000.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(5600.0, *r, 1.0);

    // 330 * 1.3 / 2.0 = 214.5 ohm.
    r = divider_r1(2.0f, 3.3f, 330.0f);
    TEST_ASSERT_TRUE(r.has_value());
    assert_close(214.5, *r, 0.05);
}

TEST_CASE("divider_r1: a shorted sender is nothing, not a divide by zero",
          "[divider]")
{
    // Vout == 0: no current flows, so there is no midpoint to measure from.
    auto r = divider_r1(0.0f, 3.3f, 1000.0f);
    TEST_ASSERT_FALSE(r.has_value());
}

TEST_CASE("divider_scale undoes a known attenuation", "[divider]")
{
    // Vin = Vout * (R1 + R2) / R2. 10k/3.3k over 1 V = 4.0303 V.
    assert_close(4.030303, divider_scale(1.0f, 10000.0f, 3300.0f), 0.001);
    // 100k/10k is a factor of 11: 2.5 V at the ADC is 27.5 V on the bus,
    // which is a 24 V system on charge.
    assert_close(27.5, divider_scale(2.5f, 100000.0f, 10000.0f), 0.001);
    // A zero lower leg is a build error; passing the reading through unchanged
    // is the least surprising thing to do with it.
    assert_close(2.5, divider_scale(2.5f, 100000.0f, 0.0f), 1e-6);
}

// ────────────────────────────────────────────────────────── frequency

TEST_CASE("frequency: counts per period to Hz", "[frequency]")
{
    // 100 counts in one second is 100 Hz.
    auto f = frequency_hz(100, 1.0f);
    TEST_ASSERT_TRUE(f.has_value());
    assert_close(100.0, *f, 1e-4);

    // 50 counts in half a second is also 100 Hz.
    f = frequency_hz(50, 0.5f);
    TEST_ASSERT_TRUE(f.has_value());
    assert_close(100.0, *f, 1e-4);
}

TEST_CASE("frequency: the pulses-per-revolution multiplier", "[frequency]")
{
    // An alternator W terminal with 6 pole pairs, 600 counts in a second:
    // 100 Hz electrical, so 100/6 = 16.667 rev/s = 1000 RPM.
    auto f = frequency_hz(600, 1.0f, 6.0f);
    TEST_ASSERT_TRUE(f.has_value());
    assert_close(100.0, *f, 1e-3);
    assert_close(6000.0, units::hz_to_rpm(*f), 0.1);

    // And the case that motivates it: 6000 counts per second on a 6-pulse
    // alternator is 1000 rev/s electrical -> 166.67 rev/s -> 10000 RPM.
    f = frequency_hz(6000, 1.0f, 6.0f);
    TEST_ASSERT_TRUE(f.has_value());
    assert_close(1000.0, *f, 0.01);
}

TEST_CASE("frequency: a zero period is nothing, not a divide by zero",
          "[frequency]")
{
    TEST_ASSERT_FALSE(frequency_hz(100, 0.0f).has_value());
    TEST_ASSERT_FALSE(frequency_hz(100, -1.0f).has_value());
    TEST_ASSERT_FALSE(frequency_hz(100, 1.0f, 0.0f).has_value());
}

// ─────────────────────────────────────────────────────── angle wrapping

TEST_CASE("wrap_angle into [0, 2pi) -- the Signal K heading interval",
          "[angle]")
{
    const float two_pi = 2.0f * units::kPi;
    assert_close(0.0, wrap_angle(0.0f), 1e-5);
    assert_close(1.0, wrap_angle(1.0f), 1e-5);
    // 2pi itself is the open end and folds to 0: a compass reads 0, never 360.
    assert_close(0.0, wrap_angle(two_pi), 1e-4);
    // Negative wraps up: -10 degrees is 350 degrees.
    assert_close(units::deg_to_rad(350.0f),
                 wrap_angle(units::deg_to_rad(-10.0f)), 1e-4);
    // Several turns out, which is what integrating a rate of turn produces.
    assert_close(units::deg_to_rad(90.0f),
                 wrap_angle(units::deg_to_rad(90.0f + 720.0f)), 1e-3);
}

TEST_CASE("wrap_angle into [-pi, pi) -- the relative-angle interval",
          "[angle]")
{
    const float min = -units::kPi;
    // 350 degrees apparent wind is 10 degrees to PORT, and the sign is the
    // information: a display saying 350 and one saying -10 mean the same thing
    // and only one of them can be steered by.
    assert_close(units::deg_to_rad(-10.0f),
                 wrap_angle(units::deg_to_rad(350.0f), min), 1e-4);
    assert_close(units::deg_to_rad(30.0f),
                 wrap_angle(units::deg_to_rad(30.0f), min), 1e-5);
    // 180 is the closed end of this interval, 181 folds to -179.
    assert_close(units::deg_to_rad(-179.0f),
                 wrap_angle(units::deg_to_rad(181.0f), min), 1e-4);
}

TEST_CASE("wrap_angle survives an absurd input without spinning", "[angle]")
{
    // A glitching sensor handing over 1e6 radians must not hold the flow task
    // in a subtract-until-in-range loop for a quarter of a million iterations.
    const float v = wrap_angle(1.0e6f);
    TEST_ASSERT_TRUE(v >= 0.0f);
    TEST_ASSERT_TRUE(v < 2.0f * units::kPi);
    TEST_ASSERT_FALSE(std::isnan(v));
}

TEST_CASE("angle_offset applies then wraps", "[angle]")
{
    // A compass mounted 12 degrees off the bow: a raw 355 plus 12 is 367,
    // which is 7.
    const float raw = units::deg_to_rad(355.0f);
    const float off = units::deg_to_rad(12.0f);
    assert_close(units::deg_to_rad(7.0f), angle_offset(raw, off), 1e-3);
    // A negative offset going the other way over zero.
    assert_close(units::deg_to_rad(353.0f),
                 angle_offset(units::deg_to_rad(5.0f),
                              units::deg_to_rad(-12.0f)),
                 1e-3);
}

// ────────────────────────────────────────────────────────── tank level

TEST_CASE("tank_level maps a US 240/33 sender to a Signal K ratio", "[tank]")
{
    // The US standard resistive sender: 240 ohm empty, 33 ohm full. Note the
    // "empty" reading is the LARGER one, which is why the arguments may be in
    // either order.
    assert_close(0.0, tank_level(240.0f, 240.0f, 33.0f), 1e-5);
    assert_close(1.0, tank_level(33.0f, 240.0f, 33.0f), 1e-5);
    // Midpoint: (136.5 - 240) / (33 - 240) = 0.5.
    assert_close(0.5, tank_level(136.5f, 240.0f, 33.0f), 1e-4);
}

TEST_CASE("tank_level clamps: a boat on its ear must not read 103%", "[tank]")
{
    // A heeled boat pushes the sender past its calibration on every tack.
    assert_close(1.0, tank_level(20.0f, 240.0f, 33.0f), 1e-5);
    assert_close(0.0, tank_level(300.0f, 240.0f, 33.0f), 1e-5);
    // And the European 0..190 direction, ascending.
    assert_close(1.0, tank_level(200.0f, 0.0f, 190.0f), 1e-5);
    assert_close(0.0, tank_level(-5.0f, 0.0f, 190.0f), 1e-5);
}

TEST_CASE("tank_level with a zero span does not divide by zero", "[tank]")
{
    // A user who has not calibrated yet, with both fields at their default.
    assert_close(0.0, tank_level(100.0f, 50.0f, 50.0f), 1e-6);
}

// ───────────────────────────────────────────────────────── battery SoC

TEST_CASE("battery SoC: AGM at its curve points", "[battery]")
{
    // The AGM curve's own samples, which are the widely published resting
    // voltages for a 12 V lead-acid bank.
    assert_close(1.0, battery_soc(12.80f, BatteryChemistry::kAgm), 0.01);
    assert_close(0.5, battery_soc(12.30f, BatteryChemistry::kAgm), 0.01);
    assert_close(0.0, battery_soc(11.60f, BatteryChemistry::kAgm), 0.01);
    // Between two points: 12.65 V is halfway between 12.6 (0.8) and 12.7
    // (0.9), so 0.85.
    assert_close(0.85, battery_soc(12.65f, BatteryChemistry::kAgm), 0.01);
}

TEST_CASE("battery SoC clamps rather than exceeding 100%", "[battery]")
{
    // 14.4 V is a battery ON CHARGE, not a battery at 140%.
    assert_close(1.0, battery_soc(14.4f, BatteryChemistry::kAgm), 0.001);
    assert_close(0.0, battery_soc(9.0f, BatteryChemistry::kAgm), 0.001);
    assert_close(1.0, battery_soc(15.0f, BatteryChemistry::kLifepo4), 0.001);
    assert_close(0.0, battery_soc(8.0f, BatteryChemistry::kLifepo4), 0.001);
}

TEST_CASE("battery SoC: LiFePO4's plateau is as flat as advertised",
          "[battery]")
{
    // The honesty check on the caveat in the header: between 13.0 V and 13.3 V
    // -- three hundred millivolts -- lies most of the battery. If this ever
    // stops being true the curve has been "improved" into a lie.
    const float lo = battery_soc(13.00f, BatteryChemistry::kLifepo4);
    const float hi = battery_soc(13.30f, BatteryChemistry::kLifepo4);
    TEST_ASSERT_TRUE(hi - lo > 0.5f);
    // And it is monotonic across the whole range, which a hand-typed table
    // easily stops being.
    float prev = -1.0f;
    for (float v = 10.0f; v <= 14.6f; v += 0.05f) {
        const float soc = battery_soc(v, BatteryChemistry::kLifepo4);
        TEST_ASSERT_TRUE(soc >= prev - 1e-4f);
        prev = soc;
    }
}

TEST_CASE("battery SoC: a 24 V bank is scaled onto the 12 V curve",
          "[battery]")
{
    // 25.6 V on a 24 V bank is 12.8 V per 12 V of nominal: full for AGM.
    assert_close(1.0, battery_soc(25.6f, BatteryChemistry::kAgm, 24.0f), 0.01);
    assert_close(0.5, battery_soc(24.6f, BatteryChemistry::kAgm, 24.0f), 0.01);
}

TEST_CASE("battery SoC: the AGM curve is monotonic", "[battery]")
{
    float prev = -1.0f;
    for (float v = 11.0f; v <= 13.5f; v += 0.02f) {
        const float soc = battery_soc(v, BatteryChemistry::kAgm);
        TEST_ASSERT_TRUE(soc >= prev - 1e-4f);
        prev = soc;
    }
}

// ─────────────────────────────────────────────────────────── thermistors

TEST_CASE("ntc_beta is exact at its reference point", "[thermistor]")
{
    // The beta equation is a two-point fit anchored at (R0, T0), so at R == R0
    // it must return T0 exactly. That is the one value it cannot be wrong
    // about, and a sign error in the logarithm shows up here immediately.
    auto t = ntc_beta_k(10000.0f, 10000.0f, 298.15f, 3950.0f);
    TEST_ASSERT_TRUE(t.has_value());
    assert_close(298.15, *t, 0.01);
}

TEST_CASE("ntc_beta: a 10k/3950 thermistor at 50 C", "[thermistor]")
{
    // Solving the beta equation by hand for T = 323.15 K gives
    // R = 10000 * exp(3950 * (1/323.15 - 1/298.15)) = 3588.18 ohm.
    // Feeding that resistance back must give 50 C.
    auto t = ntc_beta_k(3588.18f, 10000.0f, 298.15f, 3950.0f);
    TEST_ASSERT_TRUE(t.has_value());
    assert_close(323.15, *t, 0.02);
}

TEST_CASE("ntc_beta: resistance rises as temperature falls", "[thermistor]")
{
    // The defining property of an NTC. A sign error would invert this and
    // still be exact at the reference point, which is why the previous test
    // alone is not enough.
    auto cold = ntc_beta_k(30000.0f, 10000.0f, 298.15f, 3950.0f);
    auto hot = ntc_beta_k(3000.0f, 10000.0f, 298.15f, 3950.0f);
    TEST_ASSERT_TRUE(cold.has_value() && hot.has_value());
    TEST_ASSERT_TRUE(*cold < 298.15f);
    TEST_ASSERT_TRUE(*hot > 298.15f);
}

TEST_CASE("ntc_beta rejects impossible arguments", "[thermistor]")
{
    // A shorted or open sender, and a nonsense datasheet.
    TEST_ASSERT_FALSE(ntc_beta_k(0.0f, 10000.0f, 298.15f, 3950.0f).has_value());
    TEST_ASSERT_FALSE(
        ntc_beta_k(10000.0f, 0.0f, 298.15f, 3950.0f).has_value());
    TEST_ASSERT_FALSE(
        ntc_beta_k(10000.0f, 10000.0f, 298.15f, 0.0f).has_value());
}

TEST_CASE("the thermistor presets are usable tables", "[thermistor]")
{
    // Sorted ascending by resistance, so curve_eval can use them directly,
    // and descending in temperature, which is what an NTC does.
    TEST_ASSERT_TRUE(
        curve_is_sorted(kThermistorUs240_33, kThermistorUs240_33Count));
    TEST_ASSERT_TRUE(
        curve_is_sorted(kThermistorVdo180_10, kThermistorVdo180_10Count));

    // 50 ohm on the US sender is 100 C, straight off the table.
    auto t = curve_eval(kThermistorUs240_33, kThermistorUs240_33Count, 50.0f);
    TEST_ASSERT_TRUE(t.has_value());
    assert_close(373.15, *t, 0.01);

    // 22 ohm on the VDO sender is 100 C too.
    t = curve_eval(kThermistorVdo180_10, kThermistorVdo180_10Count, 22.0f);
    TEST_ASSERT_TRUE(t.has_value());
    assert_close(373.15, *t, 0.01);

    // Temperature falls with rising resistance across both tables.
    for (std::size_t i = 1; i < kThermistorUs240_33Count; i++) {
        TEST_ASSERT_TRUE(kThermistorUs240_33[i].output <
                         kThermistorUs240_33[i - 1].output);
    }
    for (std::size_t i = 1; i < kThermistorVdo180_10Count; i++) {
        TEST_ASSERT_TRUE(kThermistorVdo180_10[i].output <
                         kThermistorVdo180_10[i - 1].output);
    }
}

// ─────────────────────────────────── an end-to-end chain, in one place

TEST_CASE("a whole tank chain: volts to a Signal K ratio", "[chain]")
{
    // What a real installation computes, from ADC volts to the number the
    // plotter shows. A 1k series resistor on 3.3 V, a US 240/33 sender.
    //
    // At half full the sender is about 136.5 ohm, so
    //   Vout = 3.3 * 136.5 / (1000 + 136.5) = 0.39639 V.
    // Feed that back through and the level must come out at 0.5.
    const float v_out = 3.3f * 136.5f / (1000.0f + 136.5f);
    auto ohms = divider_r2(v_out, 3.3f, 1000.0f);
    TEST_ASSERT_TRUE(ohms.has_value());
    assert_close(136.5, *ohms, 0.1);
    assert_close(0.5, tank_level(*ohms, 240.0f, 33.0f), 0.002);
}

TEST_CASE("a whole engine chain: pulses to RPM", "[chain]")
{
    // 6-pole alternator, 300 counts in 500 ms.
    auto hz = frequency_hz(300, 0.5f, 6.0f);
    TEST_ASSERT_TRUE(hz.has_value());
    // 300 / 0.5 = 600 pulses/s / 6 = 100 rev/s = 6000 RPM.
    assert_close(100.0, *hz, 1e-3);
    assert_close(6000.0, units::hz_to_rpm(*hz), 0.1);
}

TEST_CASE("a whole cabin chain: BME280 readings to a condensation margin",
          "[chain]")
{
    // 22 C, 65% RH: the dew point is what decides whether the coachroof
    // sweats. The margin (temperature minus dew point) is the number to alarm
    // on, and it must be positive here.
    const float t = units::c_to_k(22.0f);
    auto dp = dew_point_k(t, 0.65f);
    TEST_ASSERT_TRUE(dp.has_value());
    const float margin = t - *dp;
    TEST_ASSERT_TRUE(margin > 0.0f);
    // 22 C / 65% dew point is 15.07 C, a margin of 6.93 K.
    assert_close(15.068, units::k_to_c(*dp), 0.05);
    assert_close(6.932, margin, 0.05);
}
