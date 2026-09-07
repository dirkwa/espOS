# analog_input — **Essential**

One ADC pin, read every second: calibrated with the chip's factory data,
averaged over 16 samples, scaled by two settings you edit in the web UI, and
published to a Signal K path. This is the shape of every "sensor on a wire"
device; the other examples change the peripheral, not the shape.

Replaces SensESP's `analog_input`, `repeat_sensor_analog_input` and the
tank-level tutorial.

## Wiring

| Target | Pin | Why this one |
|---|---|---|
| ESP32-C6 | GPIO 4 (ADC1_CH4) | GPIO 0..6 are ADC1 on the DevKitC |
| ESP32-P4 | GPIO 20 (ADC1_CH4) | 16..19 are ADC1 too but carry the C6 SDIO link on the Waveshare panels |

`ADC_GPIO` at the top of `main/main.c` changes it; the code maps the GPIO to
its ADC unit and channel at run time. Feed the pin 0..3.1 V, never more: a
photoresistor with a 10 kΩ resistor to ground, a tank sender behind a series
resistor, a 12 V rail through a 1:5 divider. Stay on ADC1 — on several chips
ADC2 is unusable while WiFi runs.

## What appears in Signal K

`environment.inside.illuminance` from source `espos.<hostname>`, value =
volts × `multiplier` + `offset`. With the defaults that is volts, which is
what you calibrate against: the monitor prints `0.812 V ->
environment.inside.illuminance = 0.812` on every reading. The path is a spec
path, so the server already knows its meta (units lux); this device declares
none. To publish a custom path with your own units, or a tank level as a
0..1 ratio, see the comment block in `app_main()`.

## Settings

`app.multiplier`, `app.offset`, `app.period_ms` on the web UI's Config page,
or over REST. A change is used for the next reading; no restart:

```sh
curl -X PUT http://<hostname>.local/api/v1/config -H 'Content-Type: application/json' \
     -d '{"app": {"multiplier": 250, "offset": 0}}'
```

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/analog_input
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; then approve the device in signalk-server (Security → Access
Requests). The value shows up in the server's Data Browser a second later.
