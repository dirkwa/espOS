# custom_settings — **Newbie**

An application's own settings: declared once in `main/config/app.json`,
used in C through generated key constants, applied live when the web UI
changes them, and carried across a renamed key by a migration. Replaces
SensESP's `ConfigItem`. No wiring: the "sensor" is a slow sine wave, and
the device publishes a tank level.

## The settings

| key | type | in the UI | used for |
|---|---|---|---|
| `label` | string, ≤ 32 | text field | `tanks.<tank>.0.name` |
| `interval_ms` | int 200..60000, unit `ms` | number field, range and unit as hints | the publish period |
| `enabled` | bool | checkbox | publish or not |
| `tank` | enum `freshWater` `fuel` `blackWater` `wasteWater` | drop-down | the path `tanks.<tank>.0.currentLevel` |

One JSON entry is the whole of a setting. `main/CMakeLists.txt` registers
the file (`espos_config_add_descriptor(config/app.json)`); the build
generates `espos_cfg_keys.h` — `ESPOS_CFG_NS_APP`, `ESPOS_CFG_APP_LABEL`, … —
the C tables the store validates against, and the JSON Schema the web UI
renders its form from. No key is spelled as a string literal in `main.c`.

## Where it shows

Web UI → **Config** → section **Tank sender**: the four fields. Save
`tank = fuel` and the monitor logs `Fresh water: on, every 2000 ms →
tanks.fuel.0.currentLevel` at once — the change callback
(`espos_config_subscribe`) wakes the loop, which reloads. The same document
is `GET /api/v1/config?ns=app`, and what the UI does on save is

```sh
curl -X PUT -H 'Content-Type: application/json' -d '{"app":{"interval_ms":500}}' http://<host>/api/v1/config
```

In Signal K: `tanks.freshWater.0.currentLevel` (a ratio, swinging 0.2..0.8)
and `tanks.freshWater.0.name`.

## The migration

`app.json` is at `"version": 2`. Version 1 stored the tank's name under
`name`; `main.c` registers `migrate_1_to_2()` before `espos_start()`, and
the store runs it once, on the first boot after the update, moving the
stored value to `label` and erasing `name`. Without it every device in the
field would come up with the default: a rename is a data loss unless
something moves the value. Adding a key needs no migration — it reads its
default; renaming, retyping or changing units does (docs/config.md,
"Migrations").

## Build

```sh
. $IDF_PATH/export.sh
cd components/espos_config/examples/custom_settings
idf.py set-target esp32c6 && idf.py build flash monitor
```
