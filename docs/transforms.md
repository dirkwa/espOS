# Transforms and the marine formulas

A sensor gives you a number that is almost never the number Signal K wants. A
tank sender gives ohms; the server wants a ratio from 0 to 1. An anemometer
gives a pulse rate; the server wants metres per second. A thermistor gives a
resistance on a curve nobody wrote down; the server wants kelvin. And every one
of those senders is a little bit wrong in a way that is specific to *your* boat
and can only be found with a bucket and an afternoon.

That gap is the reason to use a marine framework instead of writing C, and this
page is what fills it: a library of transform nodes for
[the data-flow graph](flow.md), and the arithmetic underneath them.

```c++
#include "espos_flow/flow.hpp"
#include "espos_flow/transforms.hpp"
using namespace espos::flow;

static Graph g;

void wire_fuel_tank() {
  auto& adc   = g.make<Poll<float, float (*)()>>("adc", 1000, read_volts);
  auto& ohms  = g.make<DividerR2>("div", 3.3f, 1000.0f);
  auto& level = g.make<Curve<>>("tank");
  auto& avg   = g.make<Ema<float>>("smooth", 0.15f);
  auto& send  = g.make<ChangeFilter<float>>("chg", 0.01f, 0.0f, 60);

  adc >> ohms >> level >> avg >> send >> fuel_out;

  level.register_config("Fuel tank", "ohms", "level");  // editable in the UI
}
```

Two components:

* **`espos_flow/transforms/`** — the nodes. Header-only, wired into a graph,
  and where a node has parameters they appear on a config page at run time.
* **`espos_formulas`** — the arithmetic. Pure floats in and out, no ESP-IDF
  dependency at all, so the [host test](development.md) checks it against
  published reference values rather than against itself.

Keeping them apart is deliberate: it is what makes the maths testable without
a device, and the nodes small enough to read at a glance.

## The one thing to get right: units

Signal K is SI, without exception. Kelvin, metres per second, radians, pascals,
cubic metres per second, ratio 0..1, revolutions per **second**.

Getting this wrong is the most common mistake in marine firmware and it is a
quiet one. A depth published in feet looks like a plausible depth, and the
plotter draws a shoal where there is none. A tank level published as `87`
instead of `0.87` reads as 8700% and looks like a server bug for a week. An
engine speed published as `3000` instead of `50` is off by exactly sixty.

So `espos::units` has every conversion a boat needs, once, `constexpr`, with
the defining constant spelled out:

| | |
|---|---|
| Temperature | `c_to_k` `k_to_c` `f_to_c` `c_to_f` `f_to_k` `k_to_f` |
| Speed | `kn_to_ms` `ms_to_kn` `kmh_to_ms` `ms_to_kmh` `mph_to_ms` |
| Angle | `deg_to_rad` `rad_to_deg` |
| Pressure | `bar_to_pa` `pa_to_bar` `psi_to_pa` `pa_to_psi` `hpa_to_pa` `pa_to_hpa` `mbar_to_pa` `inhg_to_pa` |
| Flow | `lph_to_m3s` `m3s_to_lph` `gph_to_m3s` `m3s_to_gph` `lpm_to_m3s` |
| Volume | `l_to_m3` `m3_to_l` `gal_to_m3` `m3_to_gal` `imp_gal_to_m3` |
| Distance | `ft_to_m` `m_to_ft` `fathom_to_m` `m_to_fathom` `nm_to_m` `m_to_nm` `in_to_m` |
| Ratio | `percent_to_ratio` `ratio_to_percent` |
| Rotation | `rpm_to_hz` `hz_to_rpm` |
| Charge | `ah_to_c` `c_to_ah` |

Wire one into a chain with `Convert`:

```c++
auto& kn = g.make<Convert<float, float, float (*)(float)>>(
    "kn", units::ms_to_kn);
```

Note `gal_to_m3` is the **US** gallon (3.785 L) and `imp_gal_to_m3` the
Imperial one (4.546 L). They differ by 20%, so a 50-gallon tank is either 189 L
or 227 L depending on which side of the Atlantic printed the label — which is
why they have separate names rather than one `gallon`.

## The transforms

### Arithmetic — `transforms/arithmetic.hpp`

| Node | What it does |
|---|---|
| `Linear<T>(id, mul, off)` | `in * mul + off`; both parameters live-editable |
| `Convert<In,Out,Fn>(id, fn)` | apply a pure function — the unit conversions above |
| `Cast<In,Out>(id)` | change the type visibly (the edge already converts silently) |
| `Round(id, decimals)` | round for display; never before an average |
| `Clamp<T>(id, min, max)` | limit to a range, always producing a value |
| `Integrator<T>(id, mul, initial)` | running sum — a fuel totaliser, an amp-hour counter |
| `RateOfChange<T>(id)` | the derivative, per second — rate of turn, charge rate |
| `Counter<In>(id)` | count events, ignoring their value |

`Linear` is the one every installation uses. Its multiplier and offset are the
two numbers that make a sender correct, and they are exactly the two a user
must be able to type in without a rebuild — `register_config()` puts them on a
config page as `f_<id>/mul` and `f_<id>/off`.

`Integrator` accumulates `in * mul` per input; feed it from a `Poll` with a
known period and the multiplier *is* that period. It does not look at the
clock, because the graph cannot promise a fixed arrival rate. `RateOfChange`
does look at the clock, and emits nothing on the first value — a rate needs two
samples and there is no honest number to make from one.

### Smoothing — `transforms/smoothing.hpp`

| Node | What it does |
|---|---|
| `MovingAverage<N,T>(id)` | mean of the last N; O(1) per sample |
| `Median<N,T>(id)` | median of the last N; N must be odd |
| `Ema<T>(id, alpha)` | exponential average; one float of state |
| `MinMaxHold<Slots,T>(id, window_ms)` | extremes over a rolling *time* window |

**Which one.** Sensor noise has two characters and they want different tools.
For Gaussian noise — ADC thermal noise, supply ripple — a mean is optimal.
For **outliers** — a depth sounder that sees a fish, a GPS that sees a
building, an ADC sample taken while the windlass drew 200 A — a mean is the
wrong tool entirely: one 6000 m reading in a window of five drags the average
past 1200 m, while the median does not move at all.

`Ema` should be your default for ordinary smoothing. It is one multiply-add and
one stored float against `MovingAverage`'s N floats — for a device with fifteen
smoothed channels that is 60 bytes of RAM instead of 600. Alpha ≈ 2/(N+1)
behaves like an N-sample window, so 0.2 is roughly a 9-sample average.

`MinMaxHold`'s window is time, not a sample count: a maximum gust "over the
last 60 readings" means something different at anchor than under way.

### Gates — `transforms/filters.hpp`

Half of a marine graph is deciding *not* to send something. A boat's link to
its server comes and goes, the server writes every delta to disk, and a 20 Hz
temperature channel changing in the fourth decimal is a megabyte a day of
nothing.

Four different questions, four nodes:

| Node | The question |
|---|---|
| `ChangeFilter<T>(id, min, max, max_skips)` | did the value change *enough*? |
| `Throttle<T>(id, min_interval_ms)` | has enough *time* passed? |
| `Debounce<T>(id, stable_ms)` | has it been *stable* long enough? |
| `Filter<T,Pred>(id, pred)` | is it *plausible* at all? |
| `Enable<T>(id, enabled)` | should this be flowing *at the moment*? |
| `Threshold<T>(id, min, max, in_range)` | is it inside (or outside) a range? |
| `Hysteresis<In,Out>(id, lower, upper, low, high)` | switch, without chattering |
| `Deadband<T>(id, width)` | hold the output steady while the input jitters |
| `Latch(id, latch_on)` | remember that it was true |

**`ChangeFilter`'s three arguments** each exist for a reason found the hard way:

* `min` — the change worth reporting.
* `max` — a change *larger* than this is dropped as a glitch. A depth that
  jumps 40 m between two 1 Hz samples did not happen, and passing it means the
  plotter draws a spike and the shallow alarm fires in open water. 0 disables.
* `max_skips` — after this many consecutive drops, pass the value anyway.
  Without it, a genuinely steady channel stops publishing, the server ages the
  value out, and the display goes blank on a boat where nothing is wrong. This
  is the setting people discover by losing their tank levels overnight.

espOS differs from SensESP here on purpose: the keepalive applies to the "did
not change enough" case **only**. A reading rejected as a *glitch* is never
forced through by the skip counter, because that would publish the one value
the filter exists to stop, just later.

**`ChangeFilter` vs `Deadband`.** ChangeFilter *drops* a small change, so
nothing is emitted. Deadband emits the *old* value, so the output keeps flowing
at full rate but stops jittering. Use ChangeFilter before a publisher (save
bandwidth) and Deadband before a gauge or a helm display (stop the last digit
flickering).

**`Threshold` vs `Hysteresis`.** If you are switching anything *physical*, use
Hysteresis. A bilge pump wired through a single threshold at 50 mm runs in
bursts of a tenth of a second as the water sloshes across the switch point,
which wears the pump out and flattens the battery; with `lower` at 20 mm and
`upper` at 60 mm it runs once, properly, and stops. The same applies to a
fridge compressor, an engine-temperature alarm and a low-battery cutout.

Hysteresis emits only on a *change* of state — re-announcing on every sample
would defeat the point — and a first value inside the dead band assumes LOW,
which is the safe side for a pump, a heater and a cutout alike.

**`Clamp` vs `Filter`.** Clamp produces a wrong-but-plausible number at the
limit, which downstream is indistinguishable from a real reading. Filter drops
it entirely and the server ages the channel out, which is the truth. Prefer
Filter for data and Clamp for something that must always have a value — a gauge
needle, a PWM duty cycle.

### Timing — `transforms/timing.hpp`

| Node | What it does |
|---|---|
| `Repeat<T>(id, interval_ms, mode, max_age_ms)` | re-emit the last value on a timer |
| `Expire<T>(id, max_age_ms)` | `std::optional<T>` — engaged, then `nullopt` |
| `Delay<T,Depth>(id, ms)` | pass a value on, later |
| `RunHours(id, emit_ms)` | accumulate seconds while the input is true |
| `IsoTime(id)` | the wall clock as an ISO 8601 string |
| `ParseBool(id)` / `FormatBool(id, t, f)` | between a bool and a string |

`Repeat` and `Expire` are two halves of one problem. A Signal K server ages a
value out if nothing refreshes it, so a channel that genuinely changes twice a
day disappears from the display. But a value repeated forever is a lie — a
depth still being republished ten minutes after the sounder died reads as a
working sounder. So:

* `RepeatMode::kStopAfter` (the honest default for a sensor) repeats until the
  last real input is `max_age_ms` old, then goes quiet.
* `RepeatMode::kAlways` repeats forever, for something that really is constant.
* `RepeatMode::kConstantRate` emits at a fixed cadence regardless of input.

and `Expire` turns a stale channel into an explicit `nullopt`, which an
`sk::Output<std::optional<T>>` sends as a Signal K null. Between them a display
can tell "steady" from "dead".

**`RunHours`** is engine hours, generator hours, watermaker hours — the numbers
a service schedule is written against and an owner is asked for when the boat is
sold. Two things about it matter more than the arithmetic:

* It is **stored in seconds and displayed in hours**. Seconds are the SI unit,
  so that is what goes in NVS and into a delta; the config key carries
  `displayMultiplier` of 1/3600 ([config.md](config.md)) so the page reads
  "412.5 h" and the user types hours.
* The total is **read-only in the UI** and persists across a reboot. A counter
  that resets on a flat battery is worse than none, because it is believed.
  Correcting it after a rebuild is a deliberate `set_total_s()` from code, not a
  text box a stray click can destroy.

`persist()` writes the total to NVS and is deliberately **not** called on every
tick — flash has a finite erase budget and a per-second write would exhaust it
in a season. Call it every few minutes from a `Ticker`. The same applies to
`Integrator::persist()`.

`IsoTime` reads [`espos_time`](time.md), the device's one clock. While that
clock is unsynced it emits **nothing** — publishing 1970-01-01 as a timestamp is
how an hour of data ends up in the wrong place in a log.

`ParseBool` accepts `true/false`, `on/off`, `1/0`, `yes/no` in any case, and
emits nothing for anything else. An unrecognised string is not "false"; it is a
message that was not understood, and treating it as false is how a switch panel
turns everything off when a typo arrives.

### Marine — `transforms/marine.hpp`

| Node | What it does |
|---|---|
| `Curve<MaxSamples>(id)` | piecewise-linear calibration from a UI-editable table |
| `DewPoint(id)` | temperature + humidity → dew point (Arden Buck) |
| `HeatIndex(id, effect)` | temperature + humidity → "feels like" (NOAA) |
| `AirDensity(id)` | temperature + pressure + humidity → kg/m³ |
| `DividerR2(id, v_in, r1)` | ADC volts → the sender's resistance (low side) |
| `DividerR1(id, v_in, r2)` | ADC volts → the sender's resistance (high side) |
| `DividerScale(id, r1, r2)` | undo a known attenuation — a bus-voltage monitor |
| `Frequency(id, period_ms, ppu)` | pulse counts → Hz, with pulses per revolution |
| `AngleOffset(id, offset, min)` | correct a mount angle and wrap |
| `WrapAngle(id, min)` | fold an angle back into its interval |
| `TankLevel(id, empty, full)` | sender reading → ratio 0..1, clamped |
| `BatterySoc(id, chemistry, nominal_v)` | resting volts → state of charge |

#### `Curve` — the one that finishes installations

A tank is a bent float arm over a hull-shaped void; a thermistor is an
exponential; a 1987 fuel-level sender is whatever the manufacturer felt like.
None is a straight line and none is in a datasheet you have. What you *do* have
is a bucket, a measuring jug and an afternoon.

So the sample table is **data, not code**: it is a `format: "table"` string
config key that the web UI renders as a row editor and NVS keeps across
reboots. Calibrating a tank becomes: pour in ten litres, read the raw value off
the device page, type the pair, repeat. No rebuild, no cable, no laptop.

```c++
auto& tank = g.make<Curve<>>("tank");
tank.register_config("Fuel tank", "ohms", "level");
```

The table is `[{"in":33,"out":1.0},{"in":120,"out":0.5},{"in":240,"out":0}]`;
the shorter `[[33,1.0],[120,0.5],[240,0]]` is accepted too, and rows typed out
of order are sorted. A malformed table yields the complete pairs it found
rather than nothing at all.

**Endpoints clamp; they do not extrapolate.** Below the first sample and above
the last, `Curve` returns the endpoint's output. This is a deliberate
difference from SensESP, where SensESP #1005 is what extrapolation costs: a
tank curve whose first two samples were close together produced a near-vertical
slope, an input a hair below the first sample yielded a wildly negative level,
and the division could reach 0/0 and publish a NaN. A NaN in a delta is worse
than a wrong number — it poisons averages, breaks JSON encoders that do not
quote it, and vanishes from a graph with no error anywhere.

The physics agrees with the arithmetic. A calibration table is evidence over the
range it was measured; outside that range there is no evidence, and the honest
answer is the nearest thing you actually measured. An empty tank reads empty,
not minus twelve litres. If you genuinely want extrapolation, add the samples
that describe it — which is also self-documenting.

An **empty** table emits nothing at all, rather than zero: a tank that has never
been calibrated should read as "no data", not as "empty".

#### Resistive senders

The shape of nearly every legacy marine sender: a fixed resistor and the sender
form a divider across a known supply, and the ADC reads the midpoint.

```
Vin ──[ R1 ]──┬──[ R2 ]── GND
              └── ADC (Vout)
```

`DividerR2` recovers R2 and is the one tank and temperature senders need —
they are almost always the low-side element, one terminal grounded to the hull.
`DividerR1` is for a sender in the top leg. `DividerScale` is not a sender at
all: it undoes the attenuation of a 12 V bus monitor.

All of them emit **nothing** when the reading says the sender is disconnected
(Vout at the rail for R2, Vout at zero for R1). SensESP returned infinity here,
and an open-circuit tank sender then reads as an infinite level, which a
plotter draws as a full tank. Nothing is the honest answer to a disconnected
wire.

Both parameters are live: the resistor you soldered is 1% at best and the
supply rail is not exactly 3.3 V, and both are things a user measures once and
types in.

#### `Frequency` and pulses per revolution

A tachometer sender does not give one pulse per revolution. An alternator W
terminal gives one per pole pair — typically 6 — and an aftermarket pickup
gives whatever the flywheel has teeth. So `pulses_per_unit` of 6 turns a W
terminal into engine revolutions per second, which is what
`propulsion.*.revolutions` wants. SensESP made you write a second transform for
this.

#### Angles, and the two intervals

`AngleOffset(id, offset_rad, min_rad)` corrects a mount and wraps the result.
The `min` argument picks the interval, and both matter:

* `0` gives `[0, 2π)` — Signal K's rule for a **heading** or bearing. A compass
  reads 350°; it never reads −10°.
* `-π` gives `[-π, π)` — the rule for a **relative** angle: apparent wind,
  rudder, cross-track error, where the sign is the whole point and "5° to port"
  must not become 355°.

SensESP's `AngleCorrection` did the first only, which is why every project
wiring an apparent-wind vane ended up writing the second by hand.

The offset is stored in **radians** and displayed in **degrees**
(`displayMultiplier` 180/π), so a user types "the sensor is mounted 12° off the
bow" and NVS holds 0.2094.

#### `BatterySoc`, honestly

Voltage-based state of charge is only meaningful **at rest**: no charging, no
significant load, settled long enough for surface charge to dissipate — half an
hour for AGM, hours for LiFePO4 to be worth much. Under load a battery reads
low and under charge it reads high, so the number this returns during an engine
run is a fiction.

For LiFePO4 the idea is marginal even at rest. The discharge curve is
deliberately flat: between roughly 20% and 90% the entire span is about 100 mV
per cell, so a 10 mV measurement error is 10% of the battery. A shunt-based
coulomb counter is the real answer and no arithmetic substitutes for one.

So publish it as an estimate, alarm on the ends where the curve is steep and
the reading is trustworthy, and do not budget a passage on the middle. It is
here because "roughly how full is the bank" while the boat sits on a mooring is
genuinely useful and a voltage divider can already answer it — and because
leaving it out means every project rewrites it worse.

#### Thermistor presets

`espos_formulas` ships nominal curves for the two standards most of the fleet
uses — `kThermistorUs240_33` (US 33..240 Ω) and `kThermistorVdo180_10`
(VDO/Volvo Penta 10..180 Ω) — plus `ntc_beta_k()` for a thermistor you soldered
yourself. Feed a preset into `Curve`'s constructor as a starting table, then fix
the two or three points you can verify against a known temperature: a 30-year-old
sender with a corroded earth reads high, and the point of `Curve` being editable
is that you can correct it.

## Making parameters editable

Any node with a `register_config()` gets a tab in the web UI, validation, NVS
storage and a place in the exported document, using
[`espos_config_register_ns()`](config.md#namespaces-registered-at-run-time):

```c++
auto& cal = g.make<Linear<float>>("cal", 1.7f, -0.16f);
cal.register_config("Illuminance calibration");
// -> NVS namespace f_cal, keys "mul" and "off"
```

The node id becomes the namespace `f_<id>`, so it must be **12 characters or
fewer** (NVS caps a namespace at 15). Call `register_config()` after
`espos_config_init()`; it declares the keys, registers the namespace and adopts
any stored values, so a calibration survives a reboot.

`ParamSet<N>` (in `transforms/param.hpp`) is the bridge, and it is public: your
own nodes can use it the same way.

A firmware built **without** `espos_config` still compiles and runs every
transform — `register_config()` becomes a no-op returning
`ESP_ERR_NOT_SUPPORTED` and the node keeps the values it was constructed with.
That is what the host test builds against, which is why the transform tests
need no config store.

## Where each SensESP transform went

| SensESP | espOS |
|---|---|
| `Linear` | `Linear<T>` |
| `Integrator` | `Integrator<T>` |
| `MovingAverage` | `MovingAverage<N,T>` |
| `Median` | `Median<N,T>` (N must be odd) |
| `ChangeFilter` | `ChangeFilter<T>` |
| `DebounceTemplate` | `Debounce<T>` |
| `Throttle` | `Throttle<T>` |
| `Filter` | `Filter<T,Pred>` |
| `Enable` | `Enable<T>` (plus a wirable `control()` slot) |
| `FloatThreshold`, `IntThreshold` | `Threshold<T>` |
| `Hysteresis` | `Hysteresis<In,Out>` |
| `Repeat`, `RepeatStopping`, `RepeatConstantRate`, `RepeatExpiring` | one `Repeat<T>` with a `RepeatMode` |
| `Nullable` | `Expire<T>` → `std::optional<T>` |
| `TimeCounter` | `RunHours` |
| `TimeString` | `IsoTime` |
| `CurveInterpolator` | `Curve<MaxSamples>` (clamps at the endpoints) |
| `Frequency` | `Frequency` (with pulses-per-revolution built in) |
| `AngleCorrection` | `AngleOffset` (with a choice of interval) |
| `VoltageDividerR1/R2`, `VoltageMultiplier` | `DividerR1`, `DividerR2`, `DividerScale` |
| `DewPoint` | `DewPoint` (Arden Buck, not Magnus) |
| `HeatIndexTemperature`, `HeatIndexEffect` | `HeatIndex(id, effect)` |
| `AirDensity` | `AirDensity` |
| `LambdaTransform` | `Lambda<...>` in [flow.md](flow.md) — variadic |
| `Join`, `Join3`, `Join4`, `Join5`, `Zip*` | one `Join<Ts...>` in [flow.md](flow.md) |
| `RepeatSensor` | `Poll<T>` in [flow.md](flow.md) |
| `ObservableValue` | `Value<T>` in [flow.md](flow.md) |

### Deliberately not ported

* **`ClickType` / `PressRepeater`** — deprecated upstream in favour of a button
  component. Short-press, long-press and double-click detection belongs with the
  input driver, which knows the debounce time and the pin, not in a generic
  transform that has to be told both.
* **`AnalogInput` / `AnalogVoltage`** — the ADC node gives calibrated volts
  directly (eFuse characterisation and all), so a transform that turns a raw
  count into a voltage has nothing left to do. Read volts, then use
  `DividerR2` or `Linear`.
* **`Join3` … `Join5` / `Zip*`** — one `Join<Ts...>` with `Policy::kAny` and
  `Policy::kAll` replaces the lot. SensESP #899 observed these were three
  answers to one question, each with its own age and completeness rules.
* **`SKDeltaQueue`** — buffering and retry are `espos_sk`'s job, not a node's.
  A queue in the graph would have its own idea of when the server is reachable,
  and two of those disagree.
* **`Nullable`** — replaced by `Expire<T>` producing `std::optional<T>`. A
  sentinel value meaning "no reading" is a bug waiting to be averaged;
  `std::optional` makes the absence a different *type*, so a consumer has to
  write `if (v)` and the compiler makes it.

### Additions SensESP does not have

`Clamp`, `TankLevel`, `WrapAngle`, `RateOfChange`, `Ema`, `Deadband`,
`MinMaxHold`, `Counter`, `Latch`, `Delay`, `BatterySoc`, and the `units::*`
conversion set.

## Testing

`test/host/espos_formulas_test` runs on the linux target
([development.md](development.md)) and is the real test of the arithmetic,
because `espos_formulas` has no ESP-IDF dependency to mock:

```sh
cd test/host/espos_formulas_test
../../../scripts/build.sh --preview set-target linux
../../../scripts/build.sh build
./build/espos_formulas_test.elf
```

The rule it follows is that **no expectation is checked against another espOS
function**. Each is either a defining constant spelled out longhand or a value
from an independent source:

* dew point against worked examples of the Arden Buck equation, and saturation
  vapour pressure against the steam table (2.339 kPa at 20 °C, and 101325 Pa at
  100 °C — which is what "boiling point" means);
* heat index against cells of NOAA's own published chart;
* air density against the ISA sea-level definition of 1.2250 kg/m³;
* every unit conversion against its SI definition (1852 m to the nautical mile,
  0.3048 m to the foot, 6894.757 Pa to the psi);
* dividers against algebra done by hand on ratios that come out round;
* `Curve` at, between, below and above its sample points, including the
  SensESP #1005 shape specifically.

A test that says `ms_to_kn(kn_to_ms(x)) == x` passes just as happily with the
wrong constant in both.

## See also

* [The data-flow graph](flow.md) — the nodes these plug into, and the one
  threading rule
* [Configuration](config.md) — runtime namespaces, `displayMultiplier`,
  `format: "table"`
* [Migrating from SensESP](migration-from-sensesp.md)
