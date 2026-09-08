# dusk_relay

A deck light that follows the boat's own light sensor.

This is `components/espos_sk/examples/listener_relay` written as a graph. Read
the two side by side: the C version is not obsolete, and its comments explain
what this one hides.

```cpp
auto& light = g.make<espos::sk::Listener<float>>(kLightPath, 1000);
auto& dusk  = g.make<Hysteresis<float, bool>>("dusk", 50.0f, 100.0f, true, false);
auto& relay = g.make<GpioOutput>("relay", kRelayGpio);
auto& state = g.make<espos::sk::Output<bool>>(kStatePath);

light.out() >> dusk >> relay >> state;
```

## What the C version has to build by hand

**A queue.** The subscription callback runs on the stream task and must not
block: call `espos_sk` from there and the device deadlocks against its own
socket. Here the queue is inside `Listener`, which posts into a `Mailbox`.

**A worker task**, with a stack size to choose and get right. Here it is the
flow loop every graph already has.

**The hysteresis**, as two comparisons against a `bool` that has to be
initialised to something before the first reading arrives. Here it is a node,
and it answers that question deliberately: a first value inside the dead band
assumes *low*, the safe side for a pump, a heater and a light alike.

## Why two thresholds and not one

A single threshold at 50 lux makes a cloud passing over the sensor at 50.2,
49.8, 50.1 lux switch the deck light on and off three times. Two thresholds
with a gap between them mean nothing happens inside that band, so the light
changes state at dusk and at dawn and not in between.

## Two things worth knowing before you copy it

**The value arguments are inverted.** `Hysteresis` emits `high` above the upper
limit and `low` below the lower one, and a deck light is on when it is *dark* —
so bright maps to `false` and dark to `true`. One consequence: the built-in
parameter labels from `register_config()` read "Switch on above" and "Switch
off below", which are the wrong way round for a light. Give the node its own
labels rather than confusing whoever edits them.

**Nothing republishes on a timer.** Signal K keeps the last value it was sent,
so a switch that changes twice a day is published twice a day. The C version
republishes every 10 s because it was written before a device could be asked
for its state; a `Ticker` into the same `Output` gives that behaviour back if
you want it.

## Hardware

A relay module or an LED and resistor on the pin — GPIO 10, or 22 on the
Waveshare ESP32-P4 panels, which keeps clear of the pins carrying the ESP32-C6
SDIO link.

Something on the boat has to publish `environment.outside.illuminance` for
this to do anything. Without it the graph runs and waits, which is what the
`subscribed()` check at the end of `app_main` reports.

Built for esp32c6, zero warnings. Not run on hardware.
