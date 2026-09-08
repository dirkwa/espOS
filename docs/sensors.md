# Sensors

`espos_sensors` is the hardware end of the [data-flow graph](flow.md): an ADC
pin, a switch, a pulse train, a relay, a 1-Wire temperature — each as a node
you wire into a chain.

It comes in two halves, like `espos_flow`, and either is usable without the
other:

* **The drivers** (`espos_adc.h`, `espos_gpio_in.h`, `espos_pcnt.h`,
  `espos_pwm.h`, `espos_i2c_bus.h`, `espos_onewire.h`, plain C) — thin
  helpers over the IDF 6 drivers that put the per-SoC differences in one
  place instead of in every example.
* **The nodes** (`espos_sensors/sensors.hpp`, C++) — the same drivers as
  graph nodes.

```cmake
idf_component_register(SRCS main.cpp PRIV_REQUIRES espos_core espos_flow espos_sensors)
```

## Pins are arguments, never defines

Every node takes its GPIO as a constructor parameter. A sensor library that
hard-codes GPIO 4 is one that cannot be used on the second board, and the
second board always turns up.

Sensible pins per target, for a first try:

| | ADC (ADC1 only) | free digital |
|---|---|---|
| ESP32-C6 | 0–6 | 5, 10, 21, 22 |
| ESP32-P4 | 20–23 | 20–23, 32+ |
| ESP32-C3 | 0–4 | 5–10 |
| ESP32-S3 | 1–10 | most |

Use **ADC1**. ADC2 shares hardware with the radio on several chips, and a
read then fails while WiFi is transmitting — intermittently, not at boot.

!!! warning "Waveshare ESP32-P4 panels"
    GPIO **14–19** carry the SDIO link to the ESP32-C6 co-processor that
    provides WiFi. Using one of them as a sensor pin takes the network down.
    Use 20–23.

## The nodes

| Node | What it emits | SensESP |
|---|---|---|
| `Analog` | volts, on a timer | `AnalogInput` |
| `GpioState` | `bool`, debounced, on change | `DigitalInputState` |
| `GpioChange` | `bool`, on every edge (ISR) | `DigitalInputChange` |
| `PulseCounter` | Hz, from PCNT or an ISR | `DigitalInputCounter` |
| `GpioOutput` | consumes `bool`, drives a pin | — |
| `Pwm` | consumes `float` 0..1 (LEDC) | — |
| `Ds18b20` | kelvin, from 1-Wire | `OneWireTemperature` |
| `system::*` | uptime, heap, reset reason | `SystemInfoSensor` |

### Analog

Emits **volts**, not raw counts. A count means nothing without the
attenuation and bit width that produced it, and the calibration a user types
into the web UI ("multiply by 1.7 for this divider") is only a stable number
if what it multiplies is a voltage.

```cpp
#include "espos_sensors/sensors.hpp"
#include "espos_sk_flow/sk.hpp"

using namespace espos::flow;
using namespace espos::sensors;

static Graph g;

// v * m + b — the other agent's Linear does this; a Lambda is the same thing
// written out, and is what this example uses so it depends on nothing.
static float scale(float v, float m, float b) { return v * m + b; }
using Cal = Lambda<float, float, decltype(&scale), float, float>;

extern "C" void app_main() {
  ESP_ERROR_CHECK(espos_start(nullptr));

  auto& adc = g.make<Analog>("tank", 4, 1000);          // GPIO 4, once a second
  auto& cal = g.make<Cal>("cal", scale, 0.323f, -0.15f);
  auto& out = g.make<espos::sk::Output<float>>("tanks.freshWater.0.currentLevel");

  adc >> cal >> out;
  adc.start();
  g.start();
}
```

`Analog` emits nothing when a read fails, rather than a zero — a fabricated
zero on a tank level is a false empty-tank alarm.

Calibration is automatic: whichever eFuse scheme the SoC has (curve fitting
on the C6/P4/S3, line fitting on the original ESP32) is used when the chip
carries the data. `calibrated()` says whether it does; log it once, because
a quietly uncalibrated device looks exactly like one whose divider is wrong.

### GpioState and GpioChange — two ways to read a pin

**Sample it** when the thing you are watching is slower than the poll: a
switch, a float sensor, a bilge alarm. Debouncing is a counter, and it costs
nothing at a few Hz.

```cpp
// GPIO 5, sampled every 50 ms, must hold for 4 samples (200 ms)
auto& bilge = g.make<GpioState>("bilge", 5, 50, 4);
bilge >> g.make<espos::sk::Output<bool>>("notifications.bilge.state");
bilge.start();
```

`GpioState` emits only on a **change**, plus the first sample. A switch that
has not moved is not news, and a node that re-emits every 50 ms becomes a
Signal K delta every 50 ms.

**Watch it** when an edge can be shorter than the poll — a pulse, a button
that must feel instant:

```cpp
auto& button = g.make<GpioChange<8>>("button", 6);
button.out() >> toggle;      // note .out(): the mailbox is what emits
```

`GpioChange` is worth reading if you are writing a driver of your own. Its
ISR does exactly one thing — post to a `Mailbox` — and the emit happens on
the flow task like every other node. **That is the only supported way into a
graph from an interrupt.**

### PulseCounter

Edges per second: a tachometer, a flow meter, an anemometer, a chain counter.

```cpp
static float per_rev(float hz, float ppr) { return hz / ppr; }
using PerRev = Lambda<float, float, decltype(&per_rev), float>;

auto& pulses = g.make<PulseCounter>("rpm", 21, 1000);   // GPIO 21, 1 s window
auto& rps    = g.make<PerRev>("rps", per_rev, 6.0f);    // 6 pulses per revolution
pulses >> rps >> g.make<espos::sk::Output<float>>("propulsion.main.revolutions");
pulses.start();
```

Two things this gets right that are easy to get wrong:

* **The rate uses the measured interval**, not the nominal period. Dividing
  by "1000 ms" assumes the poll ran on time, and the first poll after a
  reconnect or a flash erase is exactly the one that did not.
* **`propulsion.*.revolutions` is per second**, not per minute. It is the
  most common unit error on a tachometer and looks plausible either way until
  someone compares it with the engine's own gauge.

On a chip with a PCNT peripheral the counting and the glitch filter are done
in hardware. **The ESP32-C3 has no PCNT unit**, and there the same node counts
edges in a GPIO ISR with the glitch filter done in software. Same node, same
output; `PulseCounter::hardware()` says which you got.

### GpioOutput and Pwm

`GpioOutput` consumes a `bool` and also **emits** what the pin actually did,
so the graph can publish the truth rather than the request. They differ when
the pin failed to open — which is exactly when a dashboard must not lie.

```cpp
auto& relay = g.make<GpioOutput>("relay", 22);
auto& lamp  = g.make<Pwm>("lamp", 23);      // LEDC, 0..1 duty
lamp.set(0.4f);
```

### DS18B20 (1-Wire)

Off by default. Turn on `CONFIG_ESPOS_SENSORS_ONEWIRE` **and** add the driver
to your firmware's own manifest — it is deliberately not a dependency of
`espos_sensors`, so a firmware with no 1-Wire sensor never downloads it:

```yaml
# main/idf_component.yml
dependencies:
  espressif/ds18b20: "^0.3.1"
```

```cpp
#include "espos_sensors/onewire.hpp"

static espos::sensors::OneWireBus<4> bus(15);          // GPIO 15
static espos::sensors::Ds18b20 engine("engine", bus.handle(), 0x28ff641f2e15032dULL);

bus.add(engine);
engine >> g.make<espos::sk::Output<float>>("propulsion.main.temperature");
bus.start(2000, 11);        // convert every 2 s at 11-bit resolution
```

* **Address the sensors, never index them.** Each has a unique 64-bit ROM id;
  `bus.search()` prints them. An index renumbers every sensor after one that
  failed to answer a scan, and the engine-room reading silently becomes the
  fridge.
* **Emits kelvin**, the Signal K unit, so no path ever carries celsius by
  accident.
* **A disconnected sensor emits nothing.** The chip reads exactly 85 °C when
  its data line comes adrift, and a steady 85 °C on an engine path is the kind
  of fault that gets believed.
* **The conversion does not block.** A 12-bit conversion takes 750 ms; the bus
  broadcasts one to every sensor at once and reads them all when it is done,
  so eight sensors cost one conversion window, not eight.
* **Fit a 4.7 kΩ pull-up** from data to 3V3. The internal pull-up cannot
  supply a DS18B20 and the bus simply reads as empty — the single most common
  "my sensors do not appear".

### Any other breakout: I2C plus `Poll`

espOS does not ship a driver per breakout board and should not — there are
hundreds, they change, and a BME280 driver in this repository is one nobody
maintains. What espOS owns is the bus; the sensor is a dozen lines in your
firmware:

```cpp
static espos_i2c_dev_handle_t dev;

static std::optional<float> read_pressure() {
  uint8_t raw[3];
  if (espos_i2c_read_reg(dev, 0xF7, raw, sizeof(raw), 100) != ESP_OK)
    return std::nullopt;                       // a failed read emits nothing
  return decode(raw);                          // pascals — the Signal K unit
}

espos_i2c_bus_cfg_t bus = { .port = 0, .sda_gpio = 21, .scl_gpio = 22 };
espos_i2c_bus_open(&bus);
espos_i2c_dev_add(0, 0x76, &dev);

auto& baro = g.make<Poll<float, decltype(&read_pressure)>>("baro", 1000, read_pressure);
baro >> g.make<espos::sk::Output<float>>("environment.outside.pressure");
baro.start();
```

Returning `std::optional<float>` is the idiom for "sometimes there is
nothing": disengaged emits nothing at all, rather than a zero that a
downstream alarm has to know to ignore. `espos_i2c_probe()` is the first
thing to call when a sensor reads zeros — it says whether the device answers
at all.

## System values

`espos_sk` already publishes the standard set (uptime, free heap, RSSI …)
under `espos.<label>.*` on its own schedule. **Do not republish those paths** —
two writers on one path is a fight nobody wins. These nodes are for putting
one of those numbers on a path of your own, or alarming on it in the graph:

```cpp
auto& iram = espos::sensors::system::make_internal_free_heap(g, "iram", 10000);
iram >> low_water >> g.make<espos::sk::Notify>("lowMemory", "internal RAM low");
iram.start();
```

`free_internal()` rather than `free_heap()` is the number that matters on a
chip with PSRAM: 30 MB of free SPIRAM hides an internal pool that is nearly
gone, and it is internal RAM a WiFi buffer or a task stack has to come from.

## A switch a phone can operate

The whole point of the [inbound PUT support](signalk.md#inbound-put-control):

```cpp
auto& req   = g.make<espos::sk::PutHandler<bool>>("electrical.switches.bilge.state");
auto& relay = g.make<GpioOutput>("relay", 22);
auto& state = g.make<espos::sk::Output<bool>>("electrical.switches.bilge.state");

req.out() >> relay >> state;     // PUT -> pin -> publish what the pin did
```

The publish at the end is **not** decoration. signalk-server only routes a PUT
to a device it has seen publish that path, so a switch that never publishes
its state can never be operated. Wiring the output back through the graph is
what registers this device as the path's source.

## Testing without hardware

The four pieces of arithmetic where a wrong answer would be *silent* — the
uncalibrated ADC conversion, the counter wrap, counts-to-frequency and
pulses-per-revolution — are pure functions in `espos_sensor_math.h`, tested on
the host in `test/host/espos_sensors_test`. The drivers call them; it is not a
parallel implementation.

```console
$ cd test/host/espos_sensors_test
$ ../../../scripts/build.sh --preview set-target linux && ../../../scripts/build.sh build
$ ./build/espos_sensors_test.elf
4 Tests 0 Failures 0 Ignored
```
