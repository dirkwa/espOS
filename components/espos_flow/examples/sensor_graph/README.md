# sensor_graph

SensESP's `analog_input` example as an espOS data-flow graph: read a sensor
every 500 ms, calibrate it, publish it to Signal K.

```c++
auto& sensor = g.make<Sensor>("light", 500, read_sensor);
auto& cal    = g.make<Calibration>("cal", linear, 1.7007f, -0.165f);
auto& out    = g.make<Output>("sk", publish);

sensor >> cal >> out;
```

`Poll` is SensESP's `RepeatSensor`, the two-parameter `Lambda` is its
`Linear`, and the `Sink` is `SKOutputFloat`. See [the flow
documentation](../../../../docs/flow.md) and
[migrating from SensESP](../../../../docs/migration-from-sensesp.md).

The same device written in plain C is
[`espos_sk/examples/analog_input`](../../../espos_sk/examples/analog_input) —
the graph is sugar over exactly those calls, and both remain supported.

`read_sensor()` returns a synthetic value so the example builds for every
target without an ADC wired up; replace it with the `esp_adc` code from
`analog_input` and this is a real device.

## Build

```
idf.py set-target esp32c6
idf.py build flash monitor
```

WiFi, the Signal K server and the calibration are configuration, entered
through the portal or the web UI — nothing here knows about them.
