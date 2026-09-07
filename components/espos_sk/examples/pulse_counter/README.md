# pulse_counter — **Newbie**

Pulses on one GPIO, counted by the chip's pulse-counter peripheral with its
glitch filter, turned into revolutions per second and published to a Signal K
path; a running total is kept in the config store so it survives a reboot.
Engine RPM from a W terminal, shaft speed from a magnet, a chain counter, a
flow meter: the same loop with different settings.

Replaces SensESP's `rpm_counter`, `pcnt_rpm_counter`, `chain_counter` and
`time_counter`.

## Wiring

| Target | Pin | Why this one |
|---|---|---|
| ESP32-C6 | GPIO 5 | any free GPIO on the DevKitC |
| ESP32-P4 | GPIO 21 | header pin on the Waveshare panels, clear of the C6 SDIO link (14..19) |
| ESP32-C3 | GPIO 4 | no PCNT unit on this chip: a GPIO interrupt counts instead (no glitch filter; add an RC filter for bouncy contacts) |

`PULSE_GPIO` at the top of `main/main.c`. The internal pull-up is on, so an
open-collector sensor (hall, reed switch) connects straight to the pin and
ground. An alternator W terminal or an ignition pickup is not 3.3 V logic:
put an optocoupler or a comparator in front. The glitch filter drops pulses
shorter than 1 µs; a reed switch bouncing for milliseconds still needs an RC.

## What appears in Signal K

`propulsion.main.revolutions` in **Hz** (revolutions per second, as the spec
has it; dashboards multiply by 60). The total is not published — it is the
setting `app.total_pulses`, visible on the web UI's Config page.

## Settings

* `app.pulses_per_rev` — pulses per revolution (default 1). Hz = pulses per
  second / this.
* `app.period_ms` — publish period (default 1000). Longer is steadier at low
  pulse rates. The elapsed time is measured, not assumed.
* `app.total_pulses` — the running total. Set it to 0 to reset the counter;
  the device adopts the new value at once.

### Why the total is saved only every 60 s

Every `espos_config_set_*` is a write to NVS, which is flash: a sector takes
roughly 100 000 erases. NVS spreads writes over the partition, but with the
default 24 KB partition a write every second would use that up in about two
years; once a minute lasts longer than the boat. A reboot loses at most a
minute of pulses, which no chain counter will notice.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/pulse_counter
idf.py set-target esp32c6               # or esp32p4, esp32c3
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; then approve the device in signalk-server (Security → Access
Requests). Touch the pin to ground a few times and watch the value.
