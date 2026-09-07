# digital_switch — **Newbie**

A GPIO output (relay module, LED) whose state is a Signal K switch path,
toggled by a debounced push button. The state is published on every change
and again every 10 s, so a dashboard that came up late, or a server that was
restarted, still shows the truth without anyone pressing anything.

Replaces SensESP's `smart_switch`.

## Wiring

| Target | Output | Button | Why these |
|---|---|---|---|
| ESP32-C6 | GPIO 10 | GPIO 9 | 9 is the BOOT button every C6 devkit has, so nothing to wire for a first try |
| ESP32-P4 | GPIO 22 | GPIO 23 | header pins on the Waveshare panels, clear of the C6 SDIO link (14..19) |

`SWITCH_GPIO` and `BUTTON_GPIO` at the top of `main/main.c`. The button goes
from the pin to GND; the internal pull-up is on. The output is 3.3 V logic at
a few mA: drive a relay *module* (opto or transistor input) or a MOSFET, never
a relay coil or a pump directly. An LED with a 1 kΩ resistor is the harmless
way to see it work.

## What appears in Signal K

`electrical.switches.bilge.state`, `true`/`false`, from source
`espos.<hostname>`. `bilge` is the switch's id — name it after what it
switches. `state` is a boolean in the spec; the server knows the meta.

## What this example does not do

It does not take commands from the server. A dashboard's switch widget shows
the state but cannot flip it, because `espos_sk` has no handler yet for a PUT
request *arriving* over the stream (`espos_sk_put()` is the other direction:
this device asking the server). Inbound PUT handling lands with the data-flow
tranche; until then the button and the firmware are the only writers. Nothing
here fakes it.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/digital_switch
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; then approve the device in signalk-server (Security → Access
Requests). Press the button: the monitor prints `ON`/`OFF` and the Data
Browser follows.
