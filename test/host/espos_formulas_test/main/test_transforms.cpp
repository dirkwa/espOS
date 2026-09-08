// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// The transform NODES, driven through the real graph.
//
// The arithmetic is checked against published references in test_marine.cpp
// and test_units.cpp; what is checked here is the node behaviour that only
// exists once a value is flowing: what a ChangeFilter DOES with max_skips,
// which way a Hysteresis switches, whether a MovingAverage's ring wraps
// correctly, whether Enable's control slot actually gates.
//
// These run on the linux target against the real espos_flow, with the test's
// own task adopting the loop (espos_flow_adopt_loop), so nothing sleeps and
// nothing races. The timer-driven nodes (Debounce, Repeat, Expire, Delay) are
// exercised for their immediate behaviour only; their timers need a running
// loop, and espos_flow_test is where the wheel itself is tested.

#include <cmath>
#include <vector>

#include "unity.h"

#include "espos_flow/flow.hpp"
#include "espos_flow/transforms.hpp"

using namespace espos::flow;
namespace units = espos::units;
namespace formulas = espos::formulas;

// A recording sink. Every test wires one of these on the end of a chain and
// asserts on what arrived and in what order -- which is the only thing about
// a graph that is actually observable.
template <typename T>
struct Recorder : NodeBase, Consumer<T> {
    using consumes_type = T;
    explicit Recorder(const char *id) : NodeBase(id) {}
    void set(const T &v) override { values.push_back(v); }
    std::vector<T> values;
    std::size_t count() const { return values.size(); }
    T last() const { return values.back(); }
    void clear() { values.clear(); }
};

static void assert_close(double expected, double actual, double abs = 1e-4)
{
    TEST_ASSERT_DOUBLE_WITHIN(abs, expected, actual);
}

// ───────────────────────────────────────────────────────────── arithmetic

TEST_CASE("Linear applies multiplier then offset", "[transform]")
{
    Linear<float> lin("lin", 1.7f, -0.16f);
    Recorder<float> rec("rec");
    lin.connect_to(rec);

    lin.set(1.0f);
    lin.set(0.0f);
    lin.set(2.0f);
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
    assert_close(1.54, rec.values[0]);   // 1.0 * 1.7 - 0.16
    assert_close(-0.16, rec.values[1]);  // the offset alone
    assert_close(3.24, rec.values[2]);
}

TEST_CASE("Linear parameters are live", "[transform]")
{
    // The whole point of the node: a calibration change takes effect on the
    // NEXT reading, with no reboot and without fabricating one from the old.
    Linear<float> lin("lin", 1.0f, 0.0f);
    Recorder<float> rec("rec");
    lin.connect_to(rec);

    lin.set(10.0f);
    assert_close(10.0, rec.last());
    lin.set_multiplier(2.0f);
    // Changing a parameter must not emit by itself.
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    lin.set(10.0f);
    assert_close(20.0, rec.last());
    lin.set_offset(5.0f);
    lin.set(10.0f);
    assert_close(25.0, rec.last());
}

TEST_CASE("Clamp limits without dropping", "[transform]")
{
    Clamp<float> c("c", 0.0f, 1.0f);
    Recorder<float> rec("rec");
    c.connect_to(rec);
    c.set(-1.0f);
    c.set(0.5f);
    c.set(9.0f);
    // Three in, three out: Clamp always has a value, unlike Threshold.
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
    assert_close(0.0, rec.values[0]);
    assert_close(0.5, rec.values[1]);
    assert_close(1.0, rec.values[2]);
}

TEST_CASE("Round to a number of decimals", "[transform]")
{
    Round r("r", 2);
    Recorder<float> rec("rec");
    r.connect_to(rec);
    r.set(4.19999981f);
    assert_close(4.20, rec.last(), 1e-5);
    r.set_decimals(0);
    r.set(4.6f);
    assert_close(5.0, rec.last(), 1e-5);
    r.set(-4.6f);
    assert_close(-5.0, rec.last(), 1e-5);
}

TEST_CASE("Integrator accumulates and resets", "[transform]")
{
    // A fuel totaliser: flow rate in, multiplier is the sample interval.
    Integrator<float> in("in", 0.5f, 0.0f);
    Recorder<float> rec("rec");
    in.connect_to(rec);
    in.set(2.0f);  // += 1.0
    in.set(2.0f);  // += 1.0
    in.set(4.0f);  // += 2.0
    assert_close(4.0, in.total());
    assert_close(4.0, rec.last());
    in.reset(100.0f);
    assert_close(100.0, in.total());
    // reset() emits, so a display shows the corrected figure at once.
    assert_close(100.0, rec.last());
}

TEST_CASE("Counter counts events and ignores their value", "[transform]")
{
    Counter<bool> c("c");
    Recorder<int32_t> rec("rec");
    c.connect_to(rec);
    c.set(true);
    c.set(false);  // still an event
    c.set(true);
    TEST_ASSERT_EQUAL_INT32(3, c.count());
    TEST_ASSERT_EQUAL_INT32(3, rec.last());
    c.reset();
    TEST_ASSERT_EQUAL_INT32(0, rec.last());
}

TEST_CASE("Cast narrows a float to an int", "[transform]")
{
    Cast<float, int32_t> c("c");
    Recorder<int32_t> rec("rec");
    c.connect_to(rec);
    c.set(3.9f);
    TEST_ASSERT_EQUAL_INT32(3, rec.last());  // truncation, not rounding
    c.set(-3.9f);
    TEST_ASSERT_EQUAL_INT32(-3, rec.last());
}

TEST_CASE("Convert wires a unit conversion into a chain", "[transform]")
{
    Convert<float, float, float (*)(float)> kn("kn", units::ms_to_kn);
    Recorder<float> rec("rec");
    kn.connect_to(rec);
    kn.set(1.0f);
    assert_close(3600.0 / 1852.0, rec.last(), 1e-5);
}

// ────────────────────────────────────────────────────────────── smoothing

TEST_CASE("MovingAverage averages a partial window before it fills",
          "[transform]")
{
    // A display that shows nothing for the first N samples looks broken; the
    // average of what has arrived is a perfectly good answer.
    MovingAverage<4, float> ma("ma");
    Recorder<float> rec("rec");
    ma.connect_to(rec);
    ma.set(4.0f);
    assert_close(4.0, rec.last());
    TEST_ASSERT_FALSE(ma.full());
    ma.set(8.0f);
    assert_close(6.0, rec.last());
    ma.set(0.0f);
    assert_close(4.0, rec.last());
    ma.set(0.0f);
    assert_close(3.0, rec.last());
    TEST_ASSERT_TRUE(ma.full());
}

TEST_CASE("MovingAverage's ring drops the oldest sample", "[transform]")
{
    MovingAverage<3, float> ma("ma");
    Recorder<float> rec("rec");
    ma.connect_to(rec);
    ma.set(1.0f);
    ma.set(2.0f);
    ma.set(3.0f);
    assert_close(2.0, rec.last());  // (1+2+3)/3
    ma.set(4.0f);
    assert_close(3.0, rec.last());  // (2+3+4)/3 -- the 1 is gone
    ma.set(5.0f);
    assert_close(4.0, rec.last());  // (3+4+5)/3
    // Many wraps, so the periodic sum rebuild is exercised: a constant input
    // must give exactly that constant back, with no drift.
    for (int i = 0; i < 500; i++) ma.set(7.0f);
    assert_close(7.0, rec.last(), 1e-4);
}

TEST_CASE("Median ignores an outlier that would wreck a mean", "[transform]")
{
    // The reason both nodes exist. A depth sounder that sees one fish:
    // the mean of {5,5,5,5,6000} is 1204 m; the median is 5 m.
    Median<5, float> med("med");
    MovingAverage<5, float> ma("ma");
    Recorder<float> rmed("rm");
    Recorder<float> rma("ra");
    med.connect_to(rmed);
    ma.connect_to(rma);
    const float samples[] = { 5.0f, 5.0f, 6000.0f, 5.0f, 5.0f };
    for (float s : samples) {
        med.set(s);
        ma.set(s);
    }
    assert_close(5.0, rmed.last());
    // The mean is wrecked, by more than a thousand metres.
    TEST_ASSERT_TRUE(rma.last() > 1000.0f);
}

TEST_CASE("Median of a partial window", "[transform]")
{
    Median<5, float> med("med");
    Recorder<float> rec("rec");
    med.connect_to(rec);
    med.set(10.0f);
    assert_close(10.0, rec.last());
    med.set(20.0f);
    // Two samples: filled/2 == 1, the upper of the sorted pair.
    assert_close(20.0, rec.last());
    med.set(15.0f);
    assert_close(15.0, rec.last());
}

TEST_CASE("Ema adopts the first value rather than ramping from zero",
          "[transform]")
{
    // Starting at zero makes a temperature channel ramp up from 0 K over the
    // first minute, which looks exactly like a failing sensor.
    Ema<float> e("e", 0.5f);
    Recorder<float> rec("rec");
    e.connect_to(rec);
    e.set(300.0f);
    assert_close(300.0, rec.last());
    e.set(310.0f);
    assert_close(305.0, rec.last());  // 0.5*310 + 0.5*300
    e.set(310.0f);
    assert_close(307.5, rec.last());
}

TEST_CASE("Ema converges on a constant input", "[transform]")
{
    Ema<float> e("e", 0.1f);
    Recorder<float> rec("rec");
    e.connect_to(rec);
    e.set(0.0f);
    for (int i = 0; i < 500; i++) e.set(100.0f);
    assert_close(100.0, rec.last(), 0.01);
}

TEST_CASE("Ema's alpha is clamped to a usable range", "[transform]")
{
    // An alpha of 0 would freeze the output forever, which is not a smoothing
    // setting, it is a broken channel.
    Ema<float> e("e", 0.0f);
    TEST_ASSERT_TRUE(e.alpha() > 0.0f);
    e.set_alpha(5.0f);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, e.alpha());
}

// ──────────────────────────────────────────────────────────────── filters

TEST_CASE("ChangeFilter passes only a big enough change", "[transform]")
{
    ChangeFilter<float> cf("cf", 1.0f);
    Recorder<float> rec("rec");
    cf.connect_to(rec);
    cf.set(10.0f);  // first value always passes
    cf.set(10.5f);  // too small
    cf.set(11.5f);  // 1.5 from 10.0 -- passes
    cf.set(11.9f);  // 0.4 from 11.5 -- too small
    cf.set(13.0f);  // 1.5 from 11.5 -- passes
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
    assert_close(10.0, rec.values[0]);
    assert_close(11.5, rec.values[1]);
    assert_close(13.0, rec.values[2]);
}

TEST_CASE("ChangeFilter's max_delta rejects a glitch", "[transform]")
{
    // A depth that jumps 40 m between two 1 Hz samples did not happen.
    ChangeFilter<float> cf("cf", 0.1f, 5.0f);
    Recorder<float> rec("rec");
    cf.connect_to(rec);
    cf.set(10.0f);
    cf.set(50.0f);  // a 40 m jump: rejected
    cf.set(10.5f);  // back to reality
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    assert_close(10.0, rec.values[0]);
    assert_close(10.5, rec.values[1]);
}

TEST_CASE("ChangeFilter's max_skips keeps a steady channel alive",
          "[transform]")
{
    // Without this a genuinely steady tank level stops publishing, the server
    // ages it out, and the display goes blank on a boat where nothing is
    // wrong. This is the setting people discover by losing their tank levels
    // overnight.
    ChangeFilter<float> cf("cf", 1.0f, 0.0f, 3);
    Recorder<float> rec("rec");
    cf.connect_to(rec);
    cf.set(10.0f);  // passes (first)
    cf.set(10.1f);  // skip 1
    cf.set(10.1f);  // skip 2
    cf.set(10.1f);  // skip 3 -- reaches max_skips, passes anyway
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    assert_close(10.1, rec.values[1]);
    // And the counter restarts after a pass.
    cf.set(10.1f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
}

TEST_CASE("ChangeFilter never forces a GLITCH through on max_skips",
          "[transform]")
{
    // The deliberate difference from SensESP, and the reason max_skips is safe
    // to turn on: the keepalive applies to "did not change enough", never to
    // "this reading is impossible". Otherwise the filter would publish the one
    // value it exists to stop, just later.
    ChangeFilter<float> cf("cf", 0.1f, 5.0f, 2);
    Recorder<float> rec("rec");
    cf.connect_to(rec);
    cf.set(10.0f);
    cf.set(500.0f);  // glitch, skip 1
    cf.set(500.0f);  // glitch, skip 2 -- must NOT be forced through
    cf.set(500.0f);  // glitch, skip 3
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    assert_close(10.0, rec.values[0]);
}

TEST_CASE("Filter drops what a predicate rejects", "[transform]")
{
    Filter<float, bool (*)(float)> f(
        "f", [](float d) { return d > 0.1f && d < 200.0f; });
    Recorder<float> rec("rec");
    f.connect_to(rec);
    f.set(5.0f);
    f.set(0.0f);     // a sounder that lost bottom
    f.set(6000.0f);  // a glitch
    f.set(7.0f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    TEST_ASSERT_EQUAL_UINT32(2, f.rejected());
}

TEST_CASE("Enable gates, and its control slot drives the gate", "[transform]")
{
    Enable<float> en("en", true);
    Recorder<float> rec("rec");
    en.connect_to(rec);
    en.set(1.0f);
    en.disable();
    en.set(2.0f);
    en.set(3.0f);
    en.enable();
    en.set(4.0f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    assert_close(1.0, rec.values[0]);
    assert_close(4.0, rec.values[1]);

    // The same gate driven by a bool producer, which is what makes it wirable
    // rather than only callable.
    Value<bool> sw("sw", true);
    sw.connect_to(en.control());
    sw.set(false);
    TEST_ASSERT_FALSE(en.enabled());
    en.set(5.0f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    sw.set(true);
    en.set(6.0f);
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
}

TEST_CASE("Threshold: in-range and out-of-range polarity", "[transform]")
{
    // An engine-temperature alarm wants OUTSIDE 60..95 C, which is why the
    // polarity is a constructor argument rather than an inversion node.
    Threshold<float> alarm("al", units::c_to_k(60.0f), units::c_to_k(95.0f),
                           false);
    Recorder<bool> rec("rec");
    alarm.connect_to(rec);
    alarm.set(units::c_to_k(80.0f));
    TEST_ASSERT_FALSE(rec.last());
    alarm.set(units::c_to_k(110.0f));
    TEST_ASSERT_TRUE(rec.last());
    alarm.set(units::c_to_k(20.0f));  // stone cold is also out of range
    TEST_ASSERT_TRUE(rec.last());
    // Bounds are inclusive.
    alarm.set(units::c_to_k(95.0f));
    TEST_ASSERT_FALSE(rec.last());
    // It emits on EVERY input, so an alarm consumer keeps hearing that the
    // condition is still true.
    TEST_ASSERT_EQUAL_UINT(4, rec.count());
}

TEST_CASE("Hysteresis switches on the upper bound and off on the lower",
          "[transform]")
{
    // The bilge pump case: a single threshold at 50 mm runs the pump in bursts
    // of a tenth of a second as the water sloshes. 20/60 runs it once.
    Hysteresis<float, bool> hy("hy", 0.02f, 0.06f, false, true);
    Recorder<bool> rec("rec");
    hy.connect_to(rec);

    hy.set(0.01f);  // below lower: LOW, and this is the first state
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    TEST_ASSERT_FALSE(rec.last());

    hy.set(0.04f);  // INSIDE the band: holds LOW, emits nothing
    TEST_ASSERT_EQUAL_UINT(1, rec.count());

    hy.set(0.07f);  // above upper: HIGH
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    TEST_ASSERT_TRUE(rec.last());

    // The crucial one: falling back INTO the band must not switch off. A plain
    // threshold would chatter here on every wave.
    hy.set(0.04f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    TEST_ASSERT_TRUE(hy.state());

    hy.set(0.01f);  // below lower: LOW again
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
    TEST_ASSERT_FALSE(rec.last());
}

TEST_CASE("Hysteresis emits only on a change of state", "[transform]")
{
    Hysteresis<float, bool> hy("hy", 10.0f, 20.0f, false, true);
    Recorder<bool> rec("rec");
    hy.connect_to(rec);
    hy.set(30.0f);
    for (int i = 0; i < 50; i++) hy.set(30.0f);
    // Hysteresis exists to reduce switching; re-announcing on every sample
    // would defeat it.
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
}

TEST_CASE("Hysteresis starting inside the band assumes the safe side",
          "[transform]")
{
    // There is no previous state to hold, and guessing wrong switches
    // something on. LOW is the safe side for a pump, a heater and a cutout.
    Hysteresis<float, bool> hy("hy", 10.0f, 20.0f, false, true);
    Recorder<bool> rec("rec");
    hy.connect_to(rec);
    hy.set(15.0f);
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    TEST_ASSERT_FALSE(rec.last());
}

TEST_CASE("Deadband holds the old value instead of dropping", "[transform]")
{
    // The difference from ChangeFilter, and the reason both exist: a gauge
    // needle should sit still, not go stale.
    Deadband<float> db("db", 1.0f);
    Recorder<float> rec("rec");
    db.connect_to(rec);
    db.set(10.0f);
    db.set(10.5f);
    db.set(10.2f);
    // Three in, three OUT -- unlike ChangeFilter, which would have emitted one.
    TEST_ASSERT_EQUAL_UINT(3, rec.count());
    assert_close(10.0, rec.values[1]);
    assert_close(10.0, rec.values[2]);
    db.set(12.0f);
    assert_close(12.0, rec.last());
}

TEST_CASE("Latch remembers a momentary alarm until it is acknowledged",
          "[transform]")
{
    // A high bilge level that lasted two seconds at 03:00 is exactly the event
    // you need to know about in the morning.
    Latch l("l", true);
    Recorder<bool> rec("rec");
    l.connect_to(rec);
    l.set(false);
    TEST_ASSERT_FALSE(rec.last());
    l.set(true);
    TEST_ASSERT_TRUE(rec.last());
    l.set(false);  // the water drained; the latch does not
    TEST_ASSERT_TRUE(rec.last());
    TEST_ASSERT_TRUE(l.latched());
    l.reset();  // the acknowledge button
    TEST_ASSERT_FALSE(rec.last());
    l.set(false);
    TEST_ASSERT_FALSE(rec.last());
}

// ──────────────────────────────────────────────────────────────── timing

TEST_CASE("Expire emits an engaged optional for a real value", "[transform]")
{
    // The timer half needs a running loop; what is checked here is the shape,
    // which is the thing that replaced SensESP's sentinel-valued Nullable: the
    // absence is a different TYPE, so a consumer cannot average it by mistake.
    Expire<float> ex("ex", 60000);
    Recorder<std::optional<float>> rec("rec");
    ex.connect_to(rec);
    ex.set(4.2f);
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    TEST_ASSERT_TRUE(rec.last().has_value());
    assert_close(4.2, *rec.last());
    TEST_ASSERT_FALSE(ex.expired());
}

TEST_CASE("Repeat passes a real input straight through", "[transform]")
{
    Repeat<float> rp("rp", 30000, RepeatMode::kStopAfter, 120000);
    Recorder<float> rec("rec");
    rp.connect_to(rec);
    rp.set(1.0f);
    rp.set(2.0f);
    TEST_ASSERT_EQUAL_UINT(2, rec.count());
    assert_close(2.0, rec.last());
    TEST_ASSERT_EQUAL_UINT32(0, rp.repeats());
    rp.stop();
}

TEST_CASE("RunHours accumulates only while the input is true", "[transform]")
{
    RunHours rh("rh", 60000);
    Recorder<float> rec("rec");
    rh.connect_to(rec);

    TEST_ASSERT_FALSE(rh.running());
    assert_close(0.0, rh.total_s());

    rh.set(true);
    TEST_ASSERT_TRUE(rh.running());
    rh.set(false);
    TEST_ASSERT_FALSE(rh.running());
    // Real elapsed time here is microseconds, so the total is ~0 -- what is
    // being checked is that the state machine runs and that a stop does not
    // lose the accumulator.
    TEST_ASSERT_TRUE(rh.total_s() >= 0.0f);
    TEST_ASSERT_TRUE(rh.total_s() < 5.0f);

    // The correction path, which is deliberately code-only: the config key is
    // read-only so a web page cannot destroy the one record of engine age.
    rh.set_total_s(3600.0f * 412.5f);
    assert_close(412.5, rh.total_h(), 0.001);
    assert_close(1485000.0, rh.total_s(), 1.0);
    // Stored in seconds, displayed in hours -- the displayMultiplier of 1/3600
    // in register_config() is the same relationship.
    assert_close(rh.total_s() / 3600.0, rh.total_h(), 1e-3);
}

TEST_CASE("ParseBool accepts what people and servers actually send",
          "[transform]")
{
    ParseBool pb("pb");
    Recorder<bool> rec("rec");
    pb.connect_to(rec);
    for (const char *s : { "true", "TRUE", "on", "On", "1", "yes", "YES" }) {
        pb.set(std::string(s));
        TEST_ASSERT_TRUE(rec.last());
    }
    for (const char *s : { "false", "FALSE", "off", "OFF", "0", "no" }) {
        pb.set(std::string(s));
        TEST_ASSERT_FALSE(rec.last());
    }
    TEST_ASSERT_EQUAL_UINT(13, rec.count());
}

TEST_CASE("ParseBool emits NOTHING for an unrecognised string", "[transform]")
{
    // Treating a typo as false is how a switch panel turns everything off when
    // a malformed PUT arrives.
    ParseBool pb("pb");
    Recorder<bool> rec("rec");
    pb.connect_to(rec);
    pb.set(std::string("maybe"));
    pb.set(std::string(""));
    pb.set(std::string("2"));
    pb.set(std::string("offline"));
    TEST_ASSERT_EQUAL_UINT(0, rec.count());
}

TEST_CASE("FormatBool uses the words it was given", "[transform]")
{
    FormatBool fb("fb", "Running", "Stopped");
    Recorder<std::string> rec("rec");
    fb.connect_to(rec);
    fb.set(true);
    TEST_ASSERT_EQUAL_STRING("Running", rec.last().c_str());
    fb.set(false);
    TEST_ASSERT_EQUAL_STRING("Stopped", rec.last().c_str());
}

// ──────────────────────────────────────────────────────────────── marine

TEST_CASE("Curve node interpolates and clamps like the formula",
          "[transform]")
{
    const formulas::CurveSample tank[] = {
        { 33.0f, 1.0f }, { 120.0f, 0.5f }, { 240.0f, 0.0f }
    };
    Curve<32> c("tank", tank, 3);
    Recorder<float> rec("rec");
    c.connect_to(rec);
    c.set(120.0f);
    assert_close(0.5, rec.last());
    c.set(0.0f);  // below the table: clamps, does not extrapolate past 100%
    assert_close(1.0, rec.last());
    c.set(1000.0f);
    assert_close(0.0, rec.last());
}

TEST_CASE("Curve with no table emits nothing at all", "[transform]")
{
    // An uncalibrated tank must read as "no data", not as "empty".
    Curve<32> c("tank");
    Recorder<float> rec("rec");
    c.connect_to(rec);
    c.set(100.0f);
    TEST_ASSERT_EQUAL_UINT(0, rec.count());
    TEST_ASSERT_EQUAL_UINT(0, c.sample_count());
}

TEST_CASE("Curve parses the table config key's JSON", "[transform]")
{
    // The object form the UI's row editor produces.
    Curve<32> c("tank");
    const std::size_t n = c.set_table_json(
        "[{\"in\":33,\"out\":1.0},{\"in\":120,\"out\":0.5},"
        "{\"in\":240,\"out\":0}]");
    TEST_ASSERT_EQUAL_UINT(3, n);
    Recorder<float> rec("rec");
    c.connect_to(rec);
    c.set(120.0f);
    assert_close(0.5, rec.last());

    // The short pair form a user typing into a text box produces.
    Curve<32> c2("t2");
    TEST_ASSERT_EQUAL_UINT(3, c2.set_table_json("[[33,1.0],[120,0.5],[240,0]]"));
    Recorder<float> rec2("r2");
    c2.connect_to(rec2);
    c2.set(180.0f);
    assert_close(0.25, rec2.last());

    // Rows typed out of order are sorted, so the arithmetic still works.
    Curve<32> c3("t3");
    TEST_ASSERT_EQUAL_UINT(3, c3.set_table_json("[[240,0],[33,1.0],[120,0.5]]"));
    Recorder<float> rec3("r3");
    c3.connect_to(rec3);
    c3.set(120.0f);
    assert_close(0.5, rec3.last());
}

TEST_CASE("Curve tolerates a malformed table rather than losing everything",
          "[transform]")
{
    // A half-typed row yields the complete pairs and drops the fragment.
    Curve<32> c("tank");
    TEST_ASSERT_EQUAL_UINT(2, c.set_table_json("[[0,0],[10,1],[20"));
    TEST_ASSERT_EQUAL_UINT(0, c.set_table_json("[]"));
    TEST_ASSERT_EQUAL_UINT(0, c.set_table_json(nullptr));
    TEST_ASSERT_EQUAL_UINT(0, c.set_table_json("nonsense"));
}

TEST_CASE("Curve round-trips its table through JSON", "[transform]")
{
    Curve<32> c("tank");
    c.set_table_json("[[33,1],[120,0.5],[240,0]]");
    char buf[256];
    const std::size_t w = c.table_json(buf, sizeof(buf));
    TEST_ASSERT_TRUE(w > 0);
    Curve<32> c2("t2");
    TEST_ASSERT_EQUAL_UINT(3, c2.set_table_json(buf));
    Recorder<float> rec("rec");
    c2.connect_to(rec);
    c2.set(120.0f);
    assert_close(0.5, rec.last());
}

TEST_CASE("Curve's sample table is bounded by its template parameter",
          "[transform]")
{
    // The table lives inside the node, so an over-long config value must
    // truncate rather than write past it.
    Curve<4> c("small");
    std::string json = "[";
    for (int i = 0; i < 20; i++) {
        if (i) json += ",";
        json += "[" + std::to_string(i) + "," + std::to_string(i) + "]";
    }
    json += "]";
    TEST_ASSERT_EQUAL_UINT(4, c.set_table_json(json.c_str()));
}

TEST_CASE("DewPoint needs both inputs before it says anything", "[transform]")
{
    DewPoint dp("dp");
    Recorder<float> rec("rec");
    dp.connect_to(rec);
    dp.set(units::c_to_k(20.0f));
    // Temperature alone is not a dew point.
    TEST_ASSERT_EQUAL_UINT(0, rec.count());

    Value<float> rh("rh", 0.5f);
    rh.connect_to(dp.humidity());
    rh.set(0.5f);
    // Humidity arriving does not itself emit -- temperature drives the output,
    // so a BME280 publishing both does not double the rate.
    TEST_ASSERT_EQUAL_UINT(0, rec.count());
    dp.set(units::c_to_k(20.0f));
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    assert_close(9.2506, units::k_to_c(rec.last()), 0.01);
}

TEST_CASE("DividerR2 emits nothing for a disconnected sender", "[transform]")
{
    // The behaviour that matters on a boat: an open tank sender must not read
    // as a full tank.
    DividerR2 d("d", 3.3f, 1000.0f);
    Recorder<float> rec("rec");
    d.connect_to(rec);
    d.set(1.65f);
    assert_close(1000.0, rec.last(), 0.5);
    d.set(3.3f);  // open circuit
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    d.set(3.5f);  // noise at the rail
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
}

TEST_CASE("TankLevel node clamps a heeled boat's sender", "[transform]")
{
    TankLevel tl("tl", 240.0f, 33.0f);
    Recorder<float> rec("rec");
    tl.connect_to(rec);
    tl.set(136.5f);
    assert_close(0.5, rec.last(), 0.002);
    tl.set(10.0f);
    assert_close(1.0, rec.last());
    tl.set(300.0f);
    assert_close(0.0, rec.last());
}

TEST_CASE("AngleOffset corrects a mount and wraps the result", "[transform]")
{
    AngleOffset ao("ao", units::deg_to_rad(12.0f), 0.0f);
    Recorder<float> rec("rec");
    ao.connect_to(rec);
    ao.set(units::deg_to_rad(355.0f));
    assert_close(7.0, units::rad_to_deg(rec.last()), 0.05);
    // Into the relative interval, where the sign is the information.
    AngleOffset rel("rel", 0.0f, -units::kPi);
    Recorder<float> rr("rr");
    rel.connect_to(rr);
    rel.set(units::deg_to_rad(350.0f));
    assert_close(-10.0, units::rad_to_deg(rr.last()), 0.05);
}

TEST_CASE("Frequency node divides by period and pulses per revolution",
          "[transform]")
{
    // 6-pole alternator sampled every 500 ms.
    Frequency f("f", 500, 6.0f);
    Recorder<float> rec("rec");
    f.connect_to(rec);
    f.set(300);
    assert_close(100.0, rec.last(), 0.001);
    assert_close(6000.0, units::hz_to_rpm(rec.last()), 0.1);
    // A negative count is a counter that wrapped, not a negative rate.
    f.set(-5);
    assert_close(0.0, rec.last(), 0.001);
}

TEST_CASE("BatterySoc node maps volts to a ratio", "[transform]")
{
    BatterySoc soc("soc", formulas::BatteryChemistry::kAgm, 12.0f);
    Recorder<float> rec("rec");
    soc.connect_to(rec);
    soc.set(12.30f);
    assert_close(0.5, rec.last(), 0.01);
    soc.set(14.4f);  // on charge
    assert_close(1.0, rec.last(), 0.001);
}

// ────────────────────────────────────────────────────── a chain end to end

TEST_CASE("a whole tank chain as a graph", "[transform]")
{
    // The same arithmetic as the chain test in test_marine.cpp, but wired as
    // nodes: ADC volts -> sender resistance -> level -> smoothed -> published.
    // What this adds is that the EDGES carry it correctly and in order.
    DividerR2 div("div", 3.3f, 1000.0f);
    TankLevel tl("tl", 240.0f, 33.0f);
    Ema<float> smooth("sm", 1.0f);  // alpha 1 = no smoothing, so exact
    ChangeFilter<float> chg("chg", 0.01f);
    Recorder<float> rec("rec");

    div >> tl >> smooth >> chg >> rec;

    const float v_out = 3.3f * 136.5f / (1000.0f + 136.5f);
    div.set(v_out);
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
    assert_close(0.5, rec.last(), 0.002);

    // A tiny change is filtered out at the far end.
    div.set(v_out * 1.0005f);
    TEST_ASSERT_EQUAL_UINT(1, rec.count());

    // A disconnected sender stops the whole chain rather than publishing a
    // full tank -- the drop happens at the divider and nothing downstream
    // ever sees a fabricated value.
    div.set(3.3f);
    TEST_ASSERT_EQUAL_UINT(1, rec.count());
}

TEST_CASE("a bilge alarm chain: level -> hysteresis -> latch", "[transform]")
{
    Hysteresis<float, bool> pump("pump", 0.02f, 0.06f, false, true);
    Latch alarm("alarm", true);
    Recorder<bool> rec("rec");
    pump >> alarm >> rec;

    pump.set(0.01f);
    TEST_ASSERT_FALSE(rec.last());
    pump.set(0.07f);  // pump on
    TEST_ASSERT_TRUE(rec.last());
    pump.set(0.01f);  // pump off, but the alarm remembers it ran
    TEST_ASSERT_TRUE(rec.last());
    TEST_ASSERT_TRUE(alarm.latched());
}
