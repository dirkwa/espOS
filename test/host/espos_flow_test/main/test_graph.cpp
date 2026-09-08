// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// The typed graph, on the real runtime. Not a simulation: this links
// espos_flow itself and drives it with espos_flow_run_until_idle(), which is
// exactly what the loop task does, so what passes here is what runs on a
// device.
//
// Nothing here starts the loop task. test_main.c calls
// espos_flow_adopt_loop() first, so this task IS the loop for the run and the
// CONFIG_ESPOS_FLOW_CHECK_TASK assertion is satisfied by the truth rather than
// switched off; run_until_idle() then does the loop's work on demand.
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "unity.h"

#include "espos_flow.h"
#include "espos_flow/flow.hpp"

using namespace espos::flow;

namespace
{

// Where sinks record what they were handed, so a test can assert on the
// sequence and not only on the last value.
std::vector<float> g_floats;
std::vector<int32_t> g_ints;
std::vector<std::string> g_order;

void reset_log()
{
    g_floats.clear();
    g_ints.clear();
    g_order.clear();
}

// A Sink's callable has to be a type, and a capturing lambda's type cannot be
// named; these plain structs are what the tests wire up.
struct RecordFloat {
    void operator()(float v) const { g_floats.push_back(v); }
};
struct RecordInt {
    void operator()(int32_t v) const { g_ints.push_back(v); }
};
struct Tag {
    const char *name;
    void operator()(float) const { g_order.push_back(name); }
};

using FloatSink = Sink<float, RecordFloat>;
using IntSink = Sink<int32_t, RecordInt>;
using TagSink = Sink<float, Tag>;

// Run whatever the graph has queued. The timeout is generous: on the host
// nothing here takes a millisecond.
void pump() { espos_flow_run_until_idle(1000); }

// Drive the loop until `pred` holds or `budget_ms` of the flow clock have
// actually elapsed. A fixed iteration count is not enough: 200 passes of
// run_until_idle() on this host can take well under a millisecond, so a 1 ms
// timer may never come due and the test fails for the wrong reason.
template <typename Pred>
bool pump_until(Pred pred, uint32_t budget_ms = 200)
{
    uint32_t started = espos_flow_now_ms();
    while (!pred()) {
        pump();
        if (espos_flow_now_ms() - started >= budget_ms) return pred();
    }
    return true;
}

}  // namespace

extern "C" {

// ─────────────────────────────────────────────────────────── Value and edges

TEST_CASE("graph: a Value emits what it is set to", "[graph]")
{
    reset_log();
    Value<float> v("v");
    FloatSink out("out", RecordFloat {});
    v.connect_to(out);

    v.set(3.5f);
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(3.5f, g_floats[0]);
    TEST_ASSERT_TRUE(v.has_value());
    TEST_ASSERT_EQUAL_FLOAT(3.5f, v.get());
}

TEST_CASE("graph: a producer with no value yet says so", "[graph]")
{
    Value<float> v("v");
    TEST_ASSERT_FALSE(v.has_value());
    TEST_ASSERT_EQUAL_FLOAT(0.0f, v.get());
}

TEST_CASE("graph: connect_to returns the sink so chains read left to right",
          "[graph]")
{
    reset_log();
    Value<float> a("a");
    Symmetric<float> *unused = nullptr;
    (void)unused;
    FloatSink out("out", RecordFloat {});

    // The return value IS the sink, which is what makes a >> b >> c work.
    FloatSink &returned = a.connect_to(out);
    TEST_ASSERT_EQUAL_PTR(&out, &returned);
}

TEST_CASE("graph: operator>> is connect_to", "[graph]")
{
    reset_log();
    Value<float> a("a");
    FloatSink out("out", RecordFloat {});
    a >> out;

    a.set(1.25f);
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(1.25f, g_floats[0]);
}

TEST_CASE("graph: fan-out serves consumers in connection order", "[graph]")
{
    reset_log();
    Value<float> v("v");
    TagSink first("s1", Tag { "first" });
    TagSink second("s2", Tag { "second" });
    TagSink third("s3", Tag { "third" });
    v.connect_to(first);
    v.connect_to(second);
    v.connect_to(third);

    TEST_ASSERT_EQUAL_UINT32(3, v.edge_count());
    v.set(1.0f);
    TEST_ASSERT_EQUAL_UINT32(3, g_order.size());
    TEST_ASSERT_EQUAL_STRING("first", g_order[0].c_str());
    TEST_ASSERT_EQUAL_STRING("second", g_order[1].c_str());
    TEST_ASSERT_EQUAL_STRING("third", g_order[2].c_str());
}

TEST_CASE("graph: a chain propagates in order, left to right", "[graph]")
{
    reset_log();
    Value<float> src("src");

    // Two transforms in a row: double, then add ten.
    struct Doubler {
        float operator()(float v) const { return v * 2.0f; }
    };
    struct AddTen {
        float operator()(float v) const { return v + 10.0f; }
    };
    Lambda<float, float, Doubler> a("dbl", Doubler {});
    Lambda<float, float, AddTen> b("add", AddTen {});
    FloatSink out("out", RecordFloat {});

    src >> a >> b >> out;
    src.set(1.0f);

    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(12.0f, g_floats[0]);  // (1*2)+10, not 1+10 then *2
}

// ────────────────────────────────────────────────────── implicit conversion

TEST_CASE("graph: float into an int consumer converts in the trampoline",
          "[graph]")
{
    reset_log();
    Value<float> v("v");
    IntSink out("out", RecordInt {});
    v.connect_to(out);  // no adapter node

    v.set(3.7f);
    TEST_ASSERT_EQUAL_UINT32(1, g_ints.size());
    TEST_ASSERT_EQUAL_INT32(3, g_ints[0]);  // truncation, the C cast
}

TEST_CASE("graph: int into a float consumer converts too", "[graph]")
{
    reset_log();
    Value<int32_t> v("v");
    FloatSink out("out", RecordFloat {});
    v.connect_to(out);

    v.set(42);
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(42.0f, g_floats[0]);
}

TEST_CASE("graph: bool into a float consumer is 0 or 1", "[graph]")
{
    reset_log();
    Value<bool> v("v");
    FloatSink out("out", RecordFloat {});
    v.connect_to(out);

    v.set(true);
    v.set(false);
    TEST_ASSERT_EQUAL_UINT32(2, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_floats[0]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, g_floats[1]);
}

// ───────────────────────────────────────────────────────────────── Lambda

TEST_CASE("graph: a Lambda with no parameters transforms", "[graph][lambda]")
{
    reset_log();
    Value<float> src("src");
    struct Negate {
        float operator()(float v) const { return -v; }
    };
    Lambda<float, float, Negate> n("neg", Negate {});
    FloatSink out("out", RecordFloat {});
    src >> n >> out;

    src.set(2.5f);
    TEST_ASSERT_EQUAL_FLOAT(-2.5f, g_floats[0]);
}

TEST_CASE("graph: a Lambda carries parameters and they are live",
          "[graph][lambda]")
{
    reset_log();
    Value<float> src("src");
    // SensESP's Linear: v * m + b, with m and b editable at runtime.
    struct Linear {
        float operator()(float v, float m, float b) const { return v * m + b; }
    };
    Lambda<float, float, Linear, float, float> cal("cal", Linear {}, 2.0f, 1.0f);
    FloatSink out("out", RecordFloat {});
    src >> cal >> out;

    TEST_ASSERT_EQUAL_UINT32(2, (Lambda<float, float, Linear, float,
                                        float>::param_count));
    src.set(3.0f);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, g_floats[0]);  // 3*2+1

  // A config change moves the multiplier. It does NOT emit by itself.
    cal.param<0>() = 10.0f;
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    src.set(3.0f);
    TEST_ASSERT_EQUAL_FLOAT(31.0f, g_floats[1]);  // 3*10+1
}

TEST_CASE("graph: a Lambda with three parameters works (the variadic point)",
          "[graph][lambda]")
{
    reset_log();
    Value<float> src("src");
    struct Clamped {
        float operator()(float v, float m, float lo, float hi) const
        {
            float x = v * m;
            return x < lo ? lo : (x > hi ? hi : x);
        }
    };
    Lambda<float, float, Clamped, float, float, float> c("clamp", Clamped {}, 2.0f,
                                                         0.0f, 10.0f);
    FloatSink out("out", RecordFloat {});
    src >> c >> out;

    src.set(1.0f);
    src.set(50.0f);
    src.set(-5.0f);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_floats[0]);
    TEST_ASSERT_EQUAL_FLOAT(10.0f, g_floats[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, g_floats[2]);
}

TEST_CASE("graph: a Lambda returning optional emits only when engaged",
          "[graph][lambda]")
{
    reset_log();
    Value<float> src("src");
    // SensESP #571: "ignore an implausible reading" without a magic value.
    struct Plausible {
        std::optional<float> operator()(float v, float limit) const
        {
            if (v > limit) return std::nullopt;
            return v;
        }
    };
    Lambda<float, float, Plausible, float> f("filt", Plausible {}, 100.0f);
    FloatSink out("out", RecordFloat {});
    src >> f >> out;

    src.set(50.0f);
    src.set(500.0f);  // rejected: nothing reaches the sink
    src.set(75.0f);

    TEST_ASSERT_EQUAL_UINT32(2, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(50.0f, g_floats[0]);
    TEST_ASSERT_EQUAL_FLOAT(75.0f, g_floats[1]);
}

TEST_CASE("graph: a Lambda may change the value type", "[graph][lambda]")
{
    reset_log();
    Value<float> src("src");
    struct OverLimit {
        bool operator()(float v, float limit) const { return v > limit; }
    };
    Lambda<float, bool, OverLimit, float> alarm("alarm", OverLimit {}, 10.0f);
    struct RecordBool {
        void operator()(bool v) const { g_ints.push_back(v ? 1 : 0); }
    };
    Sink<bool, RecordBool> out("out", RecordBool {});
    src >> alarm >> out;

    src.set(5.0f);
    src.set(15.0f);
    TEST_ASSERT_EQUAL_UINT32(2, g_ints.size());
    TEST_ASSERT_EQUAL_INT32(0, g_ints[0]);
    TEST_ASSERT_EQUAL_INT32(1, g_ints[1]);
}

// ─────────────────────────────────────────────────────────────────── Join

namespace
{
std::vector<std::tuple<float, bool>> g_joined;
struct RecordJoin {
    void operator()(const std::tuple<float, bool> &t) const
    {
        g_joined.push_back(t);
    }
};
using JoinSink = Sink<std::tuple<float, bool>, RecordJoin>;
}  // namespace

TEST_CASE("graph: Join kAll waits for every input", "[graph][join]")
{
    g_joined.clear();
    Join<float, bool> j("j", 0, Policy::kAll);
    JoinSink out("out", RecordJoin {});
    j.connect_to(out);

    Value<float> depth("depth");
    Value<bool> valid("valid");
    depth.connect_to(j.in<0>());
    valid.connect_to(j.in<1>());

    depth.set(12.5f);
    TEST_ASSERT_EQUAL_UINT32(0, g_joined.size());  // half a set is no set
    valid.set(true);
    TEST_ASSERT_EQUAL_UINT32(1, g_joined.size());
    TEST_ASSERT_EQUAL_FLOAT(12.5f, std::get<0>(g_joined[0]));
    TEST_ASSERT_TRUE(std::get<1>(g_joined[0]));
}

TEST_CASE("graph: Join kAll needs a fresh set for every emit",
          "[graph][join]")
{
    g_joined.clear();
    Join<float, bool> j("j", 0, Policy::kAll);
    JoinSink out("out", RecordJoin {});
    j.connect_to(out);

    Value<float> a("a");
    Value<bool> b("b");
    a.connect_to(j.in<0>());
    b.connect_to(j.in<1>());

    a.set(1.0f);
    b.set(true);
    TEST_ASSERT_EQUAL_UINT32(1, g_joined.size());

    // One input alone does not re-emit under kAll: the set is incomplete again.
    a.set(2.0f);
    TEST_ASSERT_EQUAL_UINT32(1, g_joined.size());
    b.set(false);
    TEST_ASSERT_EQUAL_UINT32(2, g_joined.size());
    TEST_ASSERT_EQUAL_FLOAT(2.0f, std::get<0>(g_joined[1]));
}

TEST_CASE("graph: Join kAny emits on every input once all are present",
          "[graph][join]")
{
    g_joined.clear();
    Join<float, bool> j("j", 0, Policy::kAny);
    JoinSink out("out", RecordJoin {});
    j.connect_to(out);

    Value<float> a("a");
    Value<bool> b("b");
    a.connect_to(j.in<0>());
    b.connect_to(j.in<1>());

    a.set(1.0f);  // b has never been set: nothing to carry
    TEST_ASSERT_EQUAL_UINT32(0, g_joined.size());
    b.set(true);
    TEST_ASSERT_EQUAL_UINT32(1, g_joined.size());
    // From here every input emits, carrying whatever the other slot holds.
    a.set(2.0f);
    TEST_ASSERT_EQUAL_UINT32(2, g_joined.size());
    TEST_ASSERT_EQUAL_FLOAT(2.0f, std::get<0>(g_joined[1]));
    TEST_ASSERT_TRUE(std::get<1>(g_joined[1]));
}

TEST_CASE("graph: Join reports how many slots are filled", "[graph][join]")
{
    Join<float, bool> j("j", 0, Policy::kAll);
    Value<float> a("a");
    a.connect_to(j.in<0>());

    TEST_ASSERT_EQUAL_UINT32(0, j.filled());
    a.set(1.0f);
    TEST_ASSERT_EQUAL_UINT32(1, j.filled());
    TEST_ASSERT_EQUAL_UINT32(2, (Join<float, bool>::arity));
}

TEST_CASE("graph: Join with three inputs of the same type", "[graph][join]")
{
    // Two slots of the same T must not be an ambiguous base: the slot type is
    // indexed, which is what makes this compile at all.
    Join<float, float, float> j("j3", 0, Policy::kAll);
    Value<float> a("a"), b("b"), c("c");
    a.connect_to(j.in<0>());
    b.connect_to(j.in<1>());
    c.connect_to(j.in<2>());

    a.set(1.0f);
    b.set(2.0f);
    TEST_ASSERT_FALSE(j.has_value());
    c.set(3.0f);
    TEST_ASSERT_TRUE(j.has_value());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, std::get<0>(j.get()));
    TEST_ASSERT_EQUAL_FLOAT(3.0f, std::get<2>(j.get()));
}

// ────────────────────────────────────────────────────────────────── Poll

namespace
{
int g_reads = 0;
float g_next_reading = 1.0f;
struct ReadSensor {
    float operator()() const
    {
        g_reads++;
        return g_next_reading;
    }
};
int g_opt_reads = 0;
struct ReadMaybe {
    std::optional<float> operator()() const
    {
        g_opt_reads++;
        // Every other read has nothing to say: a sensor still warming up.
        if (g_opt_reads % 2 == 0) return std::nullopt;
        return 7.0f;
    }
};
}  // namespace

TEST_CASE("graph: Poll reads and emits on read_now", "[graph][poll]")
{
    reset_log();
    g_reads = 0;
    g_next_reading = 4.5f;

    Poll<float, ReadSensor> p("p", 1000, ReadSensor {});
    FloatSink out("out", RecordFloat {});
    p >> out;

    p.read_now();
    TEST_ASSERT_EQUAL_INT(1, g_reads);
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(4.5f, g_floats[0]);
}

TEST_CASE("graph: a Poll returning optional emits only when engaged",
          "[graph][poll]")
{
    reset_log();
    g_opt_reads = 0;

    Poll<float, ReadMaybe> p("p", 1000, ReadMaybe {});
    FloatSink out("out", RecordFloat {});
    p >> out;

    p.read_now();  // 1st: engaged
    p.read_now();  // 2nd: nullopt
    p.read_now();  // 3rd: engaged
    TEST_ASSERT_EQUAL_INT(3, g_opt_reads);
    TEST_ASSERT_EQUAL_UINT32(2, g_floats.size());
}

TEST_CASE("graph: a started Poll fires on the loop", "[graph][poll]")
{
    reset_log();
    g_reads = 0;
    g_next_reading = 2.0f;

    Poll<float, ReadSensor> p("p", 1, ReadSensor {});
    FloatSink out("out", RecordFloat {});
    p >> out;
    TEST_ASSERT_EQUAL(ESP_OK, p.start());

    // run_until_idle stops as soon as nothing is DUE, so a 1 ms timer needs the
    // real clock to move past its deadline before there is anything to fire.
    TEST_ASSERT_TRUE(pump_until([] { return !g_floats.empty(); }));
    p.stop();

    TEST_ASSERT_TRUE(g_floats.size() >= 1);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_floats[0]);
}

TEST_CASE("graph: stopping a Poll stops the reads", "[graph][poll]")
{
    reset_log();
    g_reads = 0;

    Poll<float, ReadSensor> p("p", 1, ReadSensor {});
    TEST_ASSERT_EQUAL(ESP_OK, p.start());
    p.stop();

    int before = g_reads;
    // Long enough that a still-armed 1 ms timer would certainly have fired.
    pump_until([] { return false; }, 20);
    TEST_ASSERT_EQUAL_INT(before, g_reads);
}

TEST_CASE("graph: a Poll's period is settable", "[graph][poll]")
{
    Poll<float, ReadSensor> p("p", 1000, ReadSensor {});
    TEST_ASSERT_EQUAL_UINT32(1000, p.period_ms());
    p.set_period(250);
    TEST_ASSERT_EQUAL_UINT32(250, p.period_ms());
    p.set_period(0);  // refused: a zero period is not a period
    TEST_ASSERT_EQUAL_UINT32(250, p.period_ms());
}

// ─────────────────────────────────────────────────── Constant and Ticker

TEST_CASE("graph: a Constant emits its value on demand", "[graph]")
{
    reset_log();
    Constant<float> c("c", 9.81f);
    FloatSink out("out", RecordFloat {});
    c >> out;

    TEST_ASSERT_EQUAL_UINT32(0, g_floats.size());  // nothing until asked
    c.emit_now();
    TEST_ASSERT_EQUAL_FLOAT(9.81f, g_floats[0]);
    TEST_ASSERT_EQUAL_FLOAT(9.81f, c.value());
}

TEST_CASE("graph: a Ticker counts its own ticks", "[graph]")
{
    Ticker t("tick", 1);
    TEST_ASSERT_EQUAL_UINT32(0, t.count());
    TEST_ASSERT_EQUAL(ESP_OK, t.start());

    TEST_ASSERT_TRUE(pump_until([&t] { return t.count() > 0; }));
    t.stop();
    TEST_ASSERT_TRUE(t.count() >= 1);
}

// ───────────────────────────────────────────────────────────────── Mailbox

TEST_CASE("graph: a Mailbox delivers on the loop, not on the poster",
          "[graph][mailbox]")
{
    reset_log();
    Mailbox<float, 4> mb("mb");
    FloatSink out("out", RecordFloat {});
    mb >> out;

    TEST_ASSERT_EQUAL(ESP_OK, mb.post(1.5f));
    // Nothing has run yet: post() only queues.
    TEST_ASSERT_EQUAL_UINT32(0, g_floats.size());
    pump();
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(1.5f, g_floats[0]);
}

TEST_CASE("graph: a Mailbox keeps posts in order", "[graph][mailbox]")
{
    reset_log();
    Mailbox<float, 8> mb("mb");
    FloatSink out("out", RecordFloat {});
    mb >> out;

    for (int i = 1; i <= 5; i++) TEST_ASSERT_EQUAL(ESP_OK, mb.post((float)i));
    pump();

    TEST_ASSERT_EQUAL_UINT32(5, g_floats.size());
    for (int i = 0; i < 5; i++) TEST_ASSERT_EQUAL_FLOAT((float)(i + 1),
                                                        g_floats[i]);
}

TEST_CASE("graph: a full Mailbox drops rather than blocks", "[graph][mailbox]")
{
    reset_log();
    Mailbox<float, 2> mb("mb");
    FloatSink out("out", RecordFloat {});
    mb >> out;

    TEST_ASSERT_EQUAL(ESP_OK, mb.post(1.0f));
    TEST_ASSERT_EQUAL(ESP_OK, mb.post(2.0f));
    // The ring holds two; the third has nowhere to go and says so at once.
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, mb.post(3.0f));
    TEST_ASSERT_EQUAL_UINT32(1, mb.dropped());

    pump();
    TEST_ASSERT_EQUAL_UINT32(2, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(1.0f, g_floats[0]);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_floats[1]);

    // Drained: it takes posts again. Pump it before leaving the scope --
    // the mailbox dies at the closing brace, and a delivery still queued
    // for it would be run by the next test's pump against an object that
    // no longer exists.
    TEST_ASSERT_EQUAL(ESP_OK, mb.post(4.0f));
    pump();
    TEST_ASSERT_EQUAL_UINT32(3, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(4.0f, g_floats[2]);
}

TEST_CASE("graph: a delivery outliving its Mailbox is dropped, not run",
          "[graph][mailbox]")
{
    pump();
    reset_log();
    {
        Mailbox<float, 2> mb("gone");
        FloatSink out("out", RecordFloat {});
        mb >> out;
        TEST_ASSERT_EQUAL(ESP_OK, mb.post(9.0f));
        // mb and out die here with that delivery still queued.
    }
    // The queued callback still runs; it must notice its mailbox is gone
    // and emit nothing rather than touch the destroyed object.
    pump();
    TEST_ASSERT_EQUAL_UINT32(0, g_floats.size());
}

TEST_CASE("graph: the ISR path delivers the same way", "[graph][mailbox]")
{
    // Drain anything an earlier test left queued before recording: the flow
    // mailbox is process-wide and a test that deliberately filled a ring can
    // leave a delivery pending.
    pump();
    reset_log();
    Mailbox<int32_t, 4> mb("mb");
    IntSink out("out", RecordInt {});
    mb >> out;

    bool woken = true;  // must be overwritten
    TEST_ASSERT_EQUAL(ESP_OK, mb.post_from_isr(7, &woken));
    TEST_ASSERT_EQUAL(ESP_OK, mb.post_from_isr(8, nullptr));
    pump();

    TEST_ASSERT_EQUAL_UINT32(2, g_ints.size());
    TEST_ASSERT_EQUAL_INT32(7, g_ints[0]);
    TEST_ASSERT_EQUAL_INT32(8, g_ints[1]);
}

TEST_CASE("graph: a Mailbox feeds a whole chain", "[graph][mailbox]")
{
    reset_log();
    Mailbox<float, 4> mb("mb");
    struct Half {
        float operator()(float v) const { return v / 2.0f; }
    };
    Lambda<float, float, Half> h("half", Half {});
    FloatSink out("out", RecordFloat {});
    mb >> h >> out;

    mb.post(10.0f);
    pump();
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
    TEST_ASSERT_EQUAL_FLOAT(5.0f, g_floats[0]);
}

// ──────────────────────────────────────────────────────── Graph ownership

TEST_CASE("graph: make() owns nodes and hands back stable references",
          "[graph][owner]")
{
    reset_log();
    Graph g;
    auto &v = g.make<Value<float>>("v", 0.0f);
    auto &out = g.make<FloatSink>("out", RecordFloat {});
    v >> out;

    TEST_ASSERT_EQUAL_UINT32(2, g.size());
    v.set(2.0f);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, g_floats[0]);

    // The reference still points at the node the graph holds. Compared through
    // NodeBase, because Value<float> derives from NodeBase AND Producer AND
    // Consumer: the derived pointer and the base pointer are legitimately
    // different addresses, and only the base one is what the list holds.
    TEST_ASSERT_EQUAL_PTR(static_cast<NodeBase *>(&v), g.first());
    TEST_ASSERT_EQUAL_STRING("v", g.first()->id());
}

TEST_CASE("graph: for_each walks adopted nodes in order", "[graph][owner]")
{
    Graph g;
    g.make<Value<float>>("one");
    g.make<Value<float>>("two");
    g.make<Value<float>>("three");

    std::vector<std::string> ids;
    g.for_each([&ids](NodeBase &n) { ids.push_back(n.id()); });
    TEST_ASSERT_EQUAL_UINT32(3, ids.size());
    TEST_ASSERT_EQUAL_STRING("one", ids[0].c_str());
    TEST_ASSERT_EQUAL_STRING("three", ids[2].c_str());
}

TEST_CASE("graph: a node may be a member and adopt itself", "[graph][owner]")
{
    reset_log();
    // The driver style: value semantics, no make<>(), no `new`.
    struct Sensor {
        Value<float> raw { "raw" };
        FloatSink out { "out", RecordFloat {} };
        explicit Sensor(Graph &g)
        {
            raw >> out;
            g.adopt(raw);
            g.adopt(out);
        }
    };
    Graph g;
    Sensor s(g);
    TEST_ASSERT_EQUAL_UINT32(2, g.size());

    s.raw.set(6.0f);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, g_floats[0]);
}

TEST_CASE("graph: an id longer than the maximum is truncated, not overrun",
          "[graph][owner]")
{
    Value<float> v("a-very-long-node-id-indeed");
    TEST_ASSERT_EQUAL_UINT32(12, strlen(v.id()));
    TEST_ASSERT_EQUAL_STRING("a-very-long-", v.id());
}

TEST_CASE("graph: a title is optional and chainable", "[graph][owner]")
{
    Value<float> v("v");
    TEST_ASSERT_EQUAL_STRING("", v.title());
    v.with_title("Coolant temperature");
    TEST_ASSERT_EQUAL_STRING("Coolant temperature", v.title());
}

// ───────────────────────────────────────────────────────────── edge pool

TEST_CASE("graph: every connect_to takes exactly one edge", "[graph][edges]")
{
    std::size_t before = edge_used();
    Value<float> v("v");
    FloatSink a("a", RecordFloat {});
    FloatSink b("b", RecordFloat {});
    v.connect_to(a);
    v.connect_to(b);
    TEST_ASSERT_EQUAL_UINT32(before + 2, edge_used());
}

TEST_CASE("graph: the edge pool has the configured capacity",
          "[graph][edges]")
{
    TEST_ASSERT_EQUAL_UINT32(CONFIG_ESPOS_FLOW_MAX_EDGES, edge_capacity());
    // Exhaustion aborts by design (a silently dropped edge is the failure this
    // avoids), so the count is what a test can assert on without killing the
    // process; edge_alloc() returning nullptr is what edge_exhausted() acts on.
    TEST_ASSERT_TRUE(edge_used() <= edge_capacity());
}

// ─────────────────────────────────────────────────────── run_until_idle

TEST_CASE("flow: run_until_idle returns true when nothing is left",
          "[flow][idle]")
{
    TEST_ASSERT_TRUE(espos_flow_run_until_idle(1000));
}

TEST_CASE("flow: run_until_idle runs what was posted", "[flow][idle]")
{
    reset_log();
    Mailbox<float, 4> mb("mb");
    FloatSink out("out", RecordFloat {});
    mb >> out;

    mb.post(3.0f);
    TEST_ASSERT_TRUE(espos_flow_run_until_idle(1000));
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
}

TEST_CASE("flow: a pending future timer still counts as idle", "[flow][idle]")
{
    // Idle means "nothing due", not "nothing scheduled": a device about to
    // sleep must not spin waiting for a timer an hour away.
    Poll<float, ReadSensor> p("p", 3600000, ReadSensor {});
    TEST_ASSERT_EQUAL(ESP_OK, p.start());
    TEST_ASSERT_TRUE(espos_flow_run_until_idle(1000));
    p.stop();
}

TEST_CASE("flow: the driving task counts as the loop", "[flow][idle]")
{
    // The wrong-task check passes for the task that IS driving the loop, which
    // is what makes a test (and a pre-sleep drain) possible at all.
    TEST_ASSERT_TRUE(espos_flow_on_loop_task());
    reset_log();
    Mailbox<float, 2> mb("mb");
    FloatSink out("out", RecordFloat {});
    mb >> out;
    mb.post(1.0f);
    espos_flow_run_until_idle(1000);
    TEST_ASSERT_EQUAL_UINT32(1, g_floats.size());
}

// ──────────────────────────────────────────────────────────────── stats

TEST_CASE("flow: stats count posts and live timers", "[flow][stats]")
{
    espos_flow_stats_t before;
    espos_flow_stats(&before);

    Mailbox<float, 4> mb("mb");
    mb.post(1.0f);
    pump();

    espos_flow_stats_t after;
    espos_flow_stats(&after);
    TEST_ASSERT_TRUE(after.posts > before.posts);
    TEST_ASSERT_TRUE(after.edges_used <= CONFIG_ESPOS_FLOW_MAX_EDGES);
}

TEST_CASE("flow: the clock moves forward", "[flow]")
{
    uint32_t a = espos_flow_now_ms();
    pump_until([] { return false; }, 5);  // burn 5 ms of real time
    uint32_t b = espos_flow_now_ms();
    TEST_ASSERT_TRUE((uint32_t)(b - a) >= 5u);
    // Modular subtraction, never a < b: the whole wrap argument in one line.
    TEST_ASSERT_TRUE((uint32_t)(b - a) < 60000u);
}

}  // extern "C"
