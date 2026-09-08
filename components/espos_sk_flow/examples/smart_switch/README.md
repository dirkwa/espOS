# smart_switch

A relay a phone can switch, and a button that switches it back.

This is the example that could not be written before the data-flow tranche:
`espos_sk` had no way to *receive* a PUT, so a device could report a switch
but never be told to change it — `components/espos_sk/examples/digital_switch`
says exactly that in a comment. The inbound half is six lines here.

```cpp
auto& put   = g.make<sk::PutHandler<bool>>(kSwitch);   // from the server
auto& relay = g.make<GpioOutput>("relay", kRelayPin);  // the pin
auto& state = g.make<sk::Output<bool>>(kSwitch);       // back to the server

put.out() >> relay >> state;
```

## The three things this example exists to show

**Publishing is what makes a switch operable.** signalk-server routes a PUT to
a device by the `(path, $source)` pairs it has seen that connection *publish*
(`processUpdates` in `src/interfaces/ws.ts`). A path this device has never
published is not a PUT target, and the server answers 405 itself without the
request ever arriving. The `>> state` at the end of the chain is what
registers this device as the path's source, and `relay.set(false)` at boot is
the first delta that does it.

**Publish what the pin did, not what was asked.** `GpioOutput` emits the state
it actually drove, so the chain publishes the truth. They differ when the pin
failed to open — exactly when a dashboard must not lie.

**Metadata is impossible to get wrong.** `electrical.switches.bilge.state` is
a specification path and gets none; the server knows what it means. The
`sensors.bilge.pumpVoltage` path is this device's own, and passing units is
only *possible* by constructing a `sk::Meta` — which is the statement "this
path is mine". See [docs/signalk.md](../../../../docs/signalk.md).

## Trying it

```console
$ idf.py set-target esp32c6 && idf.py build flash monitor
```

Then, from anywhere on the boat:

```console
$ curl -X PUT https://<server>/signalk/v1/api/vessels/self/electrical/switches/bilge/state \
    -H 'Content-Type: application/json' -d '{"value":true}'
```

The device answers `COMPLETED 200` and the relay closes. A path with no
handler answers `COMPLETED 405`, which is what signalk-server itself replies —
and what a client expects.

## Pins

Constructor arguments, never `#define`s. The defaults avoid **GPIO 14–19 on
the ESP32-P4**, which carry the SDIO link to the ESP32-C6 co-processor that
provides WiFi on the Waveshare panels — using one takes the network down.

| | relay | button | sense (ADC1) |
|---|---|---|---|
| ESP32-C6 | 10 | 9 (BOOT) | 4 |
| ESP32-P4 | 22 | 23 | 20 |

Use an **ADC1** pin for the sense input: ADC2 shares hardware with the radio
and reads fail while WiFi is transmitting.

**No hardware was attached when this example was written.** It is verified by
building for esp32c6 and esp32p4; nothing here has been run on a board.
