# health_and_led — **Newbie**

A status LED driven from the device's health table, and an application
condition the watchdog policy acts on. Replaces SensESP's `SystemStatusLed`.

## Wiring

One LED with a series resistor (≈ 330 Ω) from `LED_GPIO` (GPIO 4, a `#define`
at the top of `main.c`; an output on every supported chip, change it for your
board) to GND. The on-board LED of most devkits is an addressable RGB LED
(WS2812 on GPIO 8 on the ESP32-C6-DevKitC) and stays dark — a level does not drive it.

## What the LED shows

| LED | `espos_health_worst()` | meaning |
|---|---|---|
| off | `NORMAL` | no condition raised |
| slow blink, 1 Hz | `WARN` | something is wrong; nothing will restart |
| fast blink, 5 Hz | `ALARM` | a fault; a fatal one restarts the device after 30 s |

`espos_health_add_sink()` registers `led_sink()`, called on every change of
any condition — espOS's own (`netDown` when the router goes away, `lowMemory`)
as well as the application's. The sink stores the worst state in one word and
a 100 ms `esp_timer` does the GPIO work: sinks run on the reporting task with
no lock held, so they do the minimum and never block.

## The simulated fault, and what the policy does with it

The app polls a "sensor bus" once a second; frames arrive for 60 s
(`BUS_STOPS_AT_S`, `0` = never fail), then stop:

| uptime | `sensorBus` | LED | monitor |
|---|---|---|---|
| 0–65 s | `NORMAL` | off | |
| 65 s | `WARN` "no frames for 5 s" | slow | `sensorBus → warn (no frames for 5 s); LED shows warn` |
| 90 s | `ALARM` "no frames for 30 s", flagged `ESPOS_HEALTH_F_REBOOT_ON_ALARM` | fast | `strike 1/3: sensorBus (...)`, `2/3`, `3/3` on the next three 10 s ticks |
| ≈ 2 min | | | `restarting: sensorBus (no frames for 30 s)`, then the reboot |

The policy (docs/health.md): every `CONFIG_ESPOS_HEALTH_POLICY_TICK_S` (10 s)
it asks whether a condition raised with the flag is in `ALARM`; after
`CONFIG_ESPOS_HEALTH_POLICY_STRIKES` (3) consecutive such ticks it writes a
reset record and restarts. A `WARN` never restarts, an `ALARM` without the
flag never restarts, one clean tick resets the count, and **loss of WiFi
never restarts**: `netDown` is a `WARN` by construction, so pulling the
router makes the LED blink slowly and nothing else. Only the code that
raises a condition can flag it — reserve the flag for what a restart fixes.

After the reboot the log says `last reset was the watchdog: sensorBus (no
frames for 30 s) after 120 s, ...` and `GET /api/v1/system/info` carries the
record for the whole of that boot (`null` after any other kind of reset):
`"last_reset": {"reason": "software", "health_key": "sensorBus", "message": "no
frames for 30 s", "uptime_before_s": 120, ...}`. The condition also reaches
Signal K as `notifications.espos.<hostname>.sensorBus`: `espos_sk` is a sink too.

## Build

```sh
cd components/espos_health/examples/health_and_led && . $IDF_PATH/export.sh
idf.py set-target esp32c6 && idf.py build flash monitor      # esp32p4 builds too
```
