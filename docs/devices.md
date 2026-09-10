# Devices

`espos_devices` is whole devices rather than parts: a class that owns its
nodes and wires them in its constructor, so a firmware says what the thing
**is** rather than how it is plumbed.

```cpp
#include "espos_devices.hpp"

espos::flow::Graph g;

espos::devices::TankLevel fresh(g, {
    .id = "fresh", .gpio = 4,
    .path = "tanks.freshWater.0.currentLevel",
    .empty_ohms = 190.0f, .full_ohms = 3.0f,
});
fresh.start();
```

The same tank wired by hand is four nodes and five edges, and the wiring is
the part a firmware gets wrong: an ADC emitting volts into a transform
expecting ohms gives a tank that reads plausibly and is wrong all season.

This is the shape SensESP asked for in
[#264](https://github.com/SignalK/SensESP/issues/264) and never had.

## Nothing is hidden

Every node stays reachable — `analog()`, `level()`, `output()` — because a
device class that cannot be taken apart is a worse deal than the wiring it
replaced. Add a filter, retitle a parameter, wire a second consumer:

```cpp
auto& avg = g.make<Ema<float>>("smooth", 0.2f);
fresh.ohms() >> avg >> fresh.level();     // insert a filter mid-chain
fresh.level() >> my_alarm;                 // a second consumer
```

## What each one publishes

| Device | Publishes | The part that is easy to get wrong |
|---|---|---|
| `TankLevel` | `tanks.*.currentLevel` (ratio 0–1) | volts → ohms → ratio is three conversions, not one |
| `EngineRpm` | `propulsion.*.revolutions` | **revolutions per SECOND**, not RPM |
| `EngineHours` | `propulsion.*.runTime` | seconds, and it does **not** persist on its own |
| `SmartSwitch` | `electrical.switches.*.state` | the device must publish before the server will route a PUT to it |
| `BilgeSwitch` | the same, plus a notification | a float switch bobs; without debounce the alarm cries wolf |
| `OneWireTemperature` | `environment.*.temperature` (kelvin) | pass the ROM address, never an index |

## Three things worth reading twice

**Signal K wants SI units.** `revolutions` is revolutions per second, not RPM;
temperatures are kelvin; tank levels are a ratio, not a percentage.
Publishing 2400 where the server expects 40 looks right on a gauge that
happens to scale it and wrong everywhere else. Each device does its
conversion once, here, rather than leaving a magic constant in every
firmware.

**A switch has to publish before it can be switched.** signalk-server routes
a PUT by the `(path, $source)` pairs it has seen a device *publish*, so a
switch that only listens is one the server answers 405 for — the request
never reaches the device. `SmartSwitch` publishes its initial state in its
constructor for exactly this reason.

**`EngineHours` does not survive a reboot by itself.** `RunHours` has no
config persistence: it counts from zero at every start. Persist `total_s()`
and restore it with `set_total_s()` at boot, or accept that it resets. Said
plainly because an hour meter that silently resets is worse than none — it
reads plausibly.

## 1-Wire is opt-in

`OneWireTemperature` exists only when `CONFIG_ESPOS_SENSORS_ONEWIRE` is on,
and that needs two lines in **your** manifest, because the component manager
resolves dependencies before Kconfig exists and every firmware would
otherwise download a driver it has no sensor for:

```yaml
dependencies:
  espressif/ds18b20: "^0.3.1"
```

The bus is yours, not the device's: one wire carries many sensors and each
conversion is a bus-wide operation, so a device that owned its bus would give
you one GPIO per thermometer.

```cpp
espos::sensors::OneWireBus<4> bus(4);
espos::devices::OneWireTemperature engine(g, bus, {
    .id = "engrm", .addr = 0x28FF1234567890AB,
    .path = "environment.inside.engineRoom.temperature",
});
bus.start(5000);          // one cycle drives every sensor on the wire
```

## What is not here yet

`BatteryMonitor` (INA219), `Environment` (BME280 + dew point), `StatusLed`
and `SystemButton` need drivers or registry components that do not exist in
this tree yet. They are the same shape when they arrive; nothing above
changes.
