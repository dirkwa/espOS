# listener_relay — **Newbie**

A relay driven by a value the Signal K server streams: subscribe to
`environment.outside.illuminance`, switch a GPIO on below 50 lux and off
above 100 lux. The two thresholds are the hysteresis — a cloud passing at
60 lux changes nothing. The relay's state is published back as a switch path.

Replaces SensESP's `listener`.

## Wiring

| Target | Relay | Why this one |
|---|---|---|
| ESP32-C6 | GPIO 10 | any free GPIO on the DevKitC |
| ESP32-P4 | GPIO 22 | header pin on the Waveshare panels, clear of the C6 SDIO link (14..19) |

`RELAY_GPIO` at the top of `main/main.c`; 3.3 V logic at a few mA, so a relay
*module* or a MOSFET, never a coil directly. An LED with a 1 kΩ resistor
shows it working.

## The stream-task rule

The callback handed to `espos_sk_subscribe()` runs on the SignalK stream
task, the one task that reads the WebSocket. Three consequences shape the code:

* The strings in the update (`path`, `value_json`, …) are the parser's and
  are gone when the callback returns. Copy what you need; here `strtod()` into
  a `double`.
* Block there and every subscription on the device stalls and the stream
  drops. `xQueueSend(…, 0)` never waits — a full queue drops the value and
  logs, because a value a second late is worthless while a stalled stream
  costs everything.
* Never call an `espos_sk_*` function from it that could wait on the stream
  itself (`espos_sk_http_get()`, `espos_sk_get_value()`): a deadlock by
  construction. Do such things on the worker task, as `relay_task` may.

The worker is an ordinary FreeRTOS task with a queue. That is the whole
pattern; a display marshals to its UI thread the same way. Publishing
(`espos_sk_publish_*`) is the one thing that is fine from anywhere.

## What appears in Signal K

`electrical.switches.deckLight.state` (`true`/`false`) from
`espos.<hostname>` — the relay reporting what it did. Without a light sensor,
feed a value from the server: Admin UI → Server → Playground, paste
`{"updates":[{"values":[{"path":"environment.outside.illuminance","value":20}]}]}`
and send it; then 200 to switch off again.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/listener_relay
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; then approve the device in signalk-server (Security → Access
Requests). The subscription goes out once the stream is up, and after every reconnect.
