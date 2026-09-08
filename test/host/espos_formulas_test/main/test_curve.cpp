// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// Curve: at, between, below and above the sample points.
//
// The "below" and "above" cases are the ones that matter. SensESP #1005 was a
// NaN produced just under the lowest sample, because the code extrapolated
// from the outermost pair and could divide by zero doing it. espOS clamps, and
// these tests are the guarantee.

#include <cmath>

#include "unity.h"

#include "espos_formulas/curve.hpp"

using namespace espos::formulas;

static void assert_close(double expected, double actual, double abs = 1e-5)
{
    TEST_ASSERT_DOUBLE_WITHIN(abs, expected, actual);
}

// A tank sender curve of the shape a real one has: non-linear, because the
// tank follows the hull and the float arm swings through an arc. Resistance
// falls as the tank fills, which is the US 240/33 convention.
static const CurveSample kTank[] = {
    { 33.0f, 1.00f },
    { 70.0f, 0.75f },
    { 120.0f, 0.50f },
    { 180.0f, 0.25f },
    { 240.0f, 0.00f },
};
static constexpr std::size_t kTankN = sizeof(kTank) / sizeof(kTank[0]);

TEST_CASE("curve: exactly at each sample point returns that sample", "[curve]")
{
    for (std::size_t i = 0; i < kTankN; i++) {
        auto v = curve_eval(kTank, kTankN, kTank[i].input);
        TEST_ASSERT_TRUE(v.has_value());
        assert_close(kTank[i].output, *v);
    }
}

TEST_CASE("curve: halfway between two samples is the midpoint", "[curve]")
{
    // Between 70 (0.75) and 120 (0.50): at 95 the answer is 0.625, computed by
    // hand rather than by running the code.
    auto v = curve_eval(kTank, kTankN, 95.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.625, *v);

    // Between 180 (0.25) and 240 (0.00): at 210, 0.125.
    v = curve_eval(kTank, kTankN, 210.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.125, *v);
}

TEST_CASE("curve: a quarter of the way along an interval", "[curve]")
{
    // 33 -> 70 spans 37 ohm and 0.25 ratio. At 33 + 37/4 = 42.25 the output is
    // 1.00 - 0.25/4 = 0.9375.
    auto v = curve_eval(kTank, kTankN, 42.25f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.9375, *v);
}

TEST_CASE("curve: BELOW the first sample clamps, and is not a NaN", "[curve]")
{
    // This is SensESP #1005. Extrapolating from (33, 1.00) and (70, 0.75)
    // would give 1.081 at 21 ohm -- a tank reading 108% -- and the divide could
    // be 0/0 for a degenerate first pair. Clamping gives the first sample.
    auto v = curve_eval(kTank, kTankN, 21.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(1.00, *v);
    TEST_ASSERT_FALSE(std::isnan(*v));

    // Far below, and at zero: a shorted sender must not produce a huge number.
    v = curve_eval(kTank, kTankN, 0.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(1.00, *v);
    TEST_ASSERT_FALSE(std::isnan(*v));

    v = curve_eval(kTank, kTankN, -1000.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(1.00, *v);
}

TEST_CASE("curve: ABOVE the last sample clamps", "[curve]")
{
    // Extrapolating past (180, 0.25), (240, 0.00) gives a NEGATIVE level: at
    // 300 ohm the line reaches -0.25, and a tank cannot be minus a quarter
    // full. An open sender reads high, so this is the everyday case, not an
    // exotic one.
    auto v = curve_eval(kTank, kTankN, 300.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.00, *v);

    v = curve_eval(kTank, kTankN, 100000.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.00, *v);
}

TEST_CASE("curve: an empty table has no answer, and says so", "[curve]")
{
    // nullopt rather than 0.0: an uncalibrated tank must not read empty.
    auto v = curve_eval(kTank, 0, 100.0f);
    TEST_ASSERT_FALSE(v.has_value());
    v = curve_eval(nullptr, 0, 100.0f);
    TEST_ASSERT_FALSE(v.has_value());
}

TEST_CASE("curve: one sample is a constant everywhere", "[curve]")
{
    const CurveSample one[] = { { 50.0f, 0.42f } };
    for (float x : { -100.0f, 0.0f, 50.0f, 1000.0f }) {
        auto v = curve_eval(one, 1, x);
        TEST_ASSERT_TRUE(v.has_value());
        assert_close(0.42, *v);
    }
}

TEST_CASE("curve: duplicate inputs do not divide by zero", "[curve]")
{
    // A user typing the table can enter the same reading twice. The naive
    // interpolation is 0/0 there. The lower sample is the answer.
    const CurveSample dup[] = {
        { 0.0f, 0.0f }, { 10.0f, 1.0f }, { 10.0f, 2.0f }, { 20.0f, 3.0f }
    };
    auto v = curve_eval(dup, 4, 10.0f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    // At exactly 10 the answer is the LAST row of the duplicate run, which is
    // the one whose right-hand neighbour is a real interval -- so a query just
    // above the duplicate carries on interpolating rather than sticking.
    assert_close(2.0, *v);

    // And either side of it stays finite.
    v = curve_eval(dup, 4, 9.999f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    v = curve_eval(dup, 4, 15.0f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    assert_close(2.5, *v);

    // Three in a row, which a user who typed the same row three times
    // produces, and the walk must still terminate on a real interval.
    const CurveSample triple[] = {
        { 0.0f, 0.0f }, { 5.0f, 1.0f }, { 5.0f, 2.0f }, { 5.0f, 3.0f }, { 10.0f, 4.0f }
    };
    v = curve_eval(triple, 5, 7.5f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    // Halfway between (5, 3) and (10, 4).
    assert_close(3.5, *v);
}

TEST_CASE("curve: a duplicate run against the last sample stays finite",
          "[curve]")
{
    // The one case the forward walk cannot escape: the duplicates run into the
    // end of the table, so there is no real interval to the right. The run's
    // own output is the answer, and it must not be a NaN.
    const CurveSample tail[] = { { 0.0f, 0.0f }, { 10.0f, 1.0f }, { 10.0f, 2.0f } };
    auto v = curve_eval(tail, 3, 10.0f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    // x >= the last input, so the clamp answers before the search runs.
    assert_close(2.0, *v);

    v = curve_eval(tail, 3, 9.999f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    TEST_ASSERT_FALSE(std::isinf(*v));
}

TEST_CASE("curve: the SensESP #1005 shape specifically", "[curve]")
{
    // #1005's table had two nearly coincident samples at the low end, which
    // made the extrapolated slope enormous. Reproduce that shape and check
    // that just below the first point the answer is the first point.
    const CurveSample steep[] = {
        { 0.0f, 0.0f }, { 0.001f, 0.5f }, { 100.0f, 1.0f }
    };
    auto v = curve_eval(steep, 3, -0.0001f);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_FALSE(std::isnan(*v));
    TEST_ASSERT_FALSE(std::isinf(*v));
    assert_close(0.0, *v);

    // And exactly at zero, which is where an ADC sits with nothing connected.
    v = curve_eval(steep, 3, 0.0f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(0.0, *v);
}

TEST_CASE("curve: binary search finds the right interval in a long table",
          "[curve]")
{
    // 64 samples of y = 2x, so every interpolated point has a known answer and
    // an off-by-one in the search shows up immediately.
    CurveSample many[64];
    for (int i = 0; i < 64; i++) {
        many[i].input = static_cast<float>(i);
        many[i].output = static_cast<float>(2 * i);
    }
    for (int i = 0; i < 63; i++) {
        const float x = static_cast<float>(i) + 0.5f;
        auto v = curve_eval(many, 64, x);
        TEST_ASSERT_TRUE(v.has_value());
        assert_close(2.0 * x, *v, 1e-4);
    }
    // The endpoints of the long table, too.
    assert_close(0.0, *curve_eval(many, 64, 0.0f));
    assert_close(126.0, *curve_eval(many, 64, 63.0f));
    assert_close(126.0, *curve_eval(many, 64, 99.0f));  // clamped
}

TEST_CASE("curve: a descending-output table interpolates the same", "[curve]")
{
    // The tank table above already falls; this checks a rising input with a
    // falling output over a thermistor's shape, where nothing about the
    // algorithm may assume the output increases.
    const CurveSample ntc[] = {
        { 33.0f, 393.15f }, { 50.0f, 373.15f }, { 240.0f, 313.15f }
    };
    // Halfway between 33 and 50 is 41.5 ohm; halfway between 393.15 and 373.15
    // is 383.15 K.
    auto v = curve_eval(ntc, 3, 41.5f);
    TEST_ASSERT_TRUE(v.has_value());
    assert_close(383.15, *v, 1e-3);
}

TEST_CASE("curve_is_sorted detects an out-of-order table", "[curve]")
{
    TEST_ASSERT_TRUE(curve_is_sorted(kTank, kTankN));
    const CurveSample bad[] = { { 10.0f, 1.0f }, { 5.0f, 2.0f }, { 20.0f, 3.0f } };
    TEST_ASSERT_FALSE(curve_is_sorted(bad, 3));
    // Equal inputs are not strictly ascending either -- the table editor
    // should reject them, and curve_eval survives them regardless.
    const CurveSample eq[] = { { 10.0f, 1.0f }, { 10.0f, 2.0f } };
    TEST_ASSERT_FALSE(curve_is_sorted(eq, 2));
    TEST_ASSERT_TRUE(curve_is_sorted(nullptr, 0));
}

TEST_CASE("curve_sort puts a user's typed-in table in order", "[curve]")
{
    // What a user actually produces: rows typed as they measured them, which
    // is emptying order -- descending.
    CurveSample typed[] = {
        { 240.0f, 0.00f }, { 33.0f, 1.00f }, { 120.0f, 0.50f }, { 70.0f, 0.75f }, { 180.0f, 0.25f }
    };
    curve_sort(typed, 5);
    TEST_ASSERT_TRUE(curve_is_sorted(typed, 5));
    // And the pairing survived the sort: 120 must still map to 0.50.
    for (std::size_t i = 0; i < 5; i++) {
        auto v = curve_eval(typed, 5, kTank[i].input);
        TEST_ASSERT_TRUE(v.has_value());
        assert_close(kTank[i].output, *v);
    }
}

TEST_CASE("curve_eval_or substitutes only for an empty table", "[curve]")
{
    TEST_ASSERT_EQUAL_FLOAT(-1.0f, curve_eval_or(kTank, 0, 100.0f, -1.0f));
    assert_close(0.625, curve_eval_or(kTank, kTankN, 95.0f, -1.0f));
}
