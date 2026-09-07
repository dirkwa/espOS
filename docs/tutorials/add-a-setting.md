# Tutorial: add a setting

**Newbie.** A number your code uses becomes a field on the Config page, a key
in the REST document and a validated NVS value — by writing one JSON entry.
Descriptor key → generated constant → change callback → the UI, in that order.
Start where [first-sensor](first-sensor.md) ends (any project that publishes a
value will do); `components/espos_config/examples/custom_settings` is the finished form.

## 1. The descriptor

A setting is declared once, in a JSON descriptor per NVS namespace
([config.md](../config.md)). Create `main/config/light.json`:

```json
{
  "namespace": "light",
  "version": 1,
  "title": "Light sensor",
  "keys": [
    {"name": "multiplier", "type": "float", "default": 1.0, "min": -100, "max": 100,
     "title": "Multiplier", "description": "Reading is multiplied by this before publishing."},
    {"name": "offset", "type": "float", "default": 0.0, "min": -10, "max": 10, "title": "Offset"},
    {"name": "period_ms", "type": "int", "default": 500, "min": 100, "max": 60000, "unit": "ms",
     "title": "Read interval"},
    {"name": "enabled", "type": "bool", "default": true, "title": "Publish"}
  ]
}
```

Namespace and key names are NVS identifiers: lower case, at most 15
characters. `default` must satisfy the constraints you give — the generator
checks that at build time. Register the file in `main/CMakeLists.txt`, after
`idf_component_register`, and add `espos_config` to `PRIV_REQUIRES`:

```cmake
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_config espos_sk esp_adc)
espos_config_add_descriptor(config/light.json)
```

## 2. The generated constants

Build once (`espos/scripts/build.sh build`). The generator merged every
registered descriptor — espOS's own `wifi`, `httpd`, `sk`, `ota` and now
`light` — into `build/esp-idf/espos_config/gen/include/espos_cfg_keys.h`:
`ESPOS_CFG_NS_LIGHT` is `"light"`, `ESPOS_CFG_LIGHT_MULTIPLIER` is
`"multiplier"`, one constant per key; plus the validation tables writes are
checked against and the JSON Schema served at `GET /api/v1/config/schema`.
Code never spells an NVS key by hand: a typo in a constant is a compile error,
a typo in a string literal is a default silently read back. A duplicate
namespace or an over-long name fails the build with the file named.

## 3. Read it, and follow changes

Include `espos_cfg_keys.h` and `espos_config.h`. Reads never fail for a
declared key — a missing or corrupt stored value reads as the default — so
loading is four calls, and the same function serves as the change callback:

```c
static float s_mult = 1.0f, s_offset = 0.0f;   /* word-sized: written by the config caller, read by the loop */
static int32_t s_period_ms = 500;
static bool s_enabled = true;

static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns; (void)key; (void)arg;
    espos_config_get_float(ESPOS_CFG_NS_LIGHT, ESPOS_CFG_LIGHT_MULTIPLIER, &s_mult);
    espos_config_get_float(ESPOS_CFG_NS_LIGHT, ESPOS_CFG_LIGHT_OFFSET, &s_offset);
    espos_config_get_i32(ESPOS_CFG_NS_LIGHT, ESPOS_CFG_LIGHT_PERIOD_MS, &s_period_ms);
    espos_config_get_bool(ESPOS_CFG_NS_LIGHT, ESPOS_CFG_LIGHT_ENABLED, &s_enabled);
}
```

In `app_main()`, after `espos_start()`:

```c
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    for (;; vTaskDelay(pdMS_TO_TICKS(s_period_ms))) {
        /* … read mv as in first-sensor … */
        if (s_enabled) {
            espos_sk_publish_number(PATH, mv / 3300.0 * s_mult + s_offset);
        }
    }
```

The callback runs once per changed key, on the task that wrote — an HTTP
handler for a `PUT`, your own task for `espos_config_set_*()` — with the store
lock released, so it may read config freely. Keep it short and the shared
state word-sized; a string setting is read where it is used, or copied under a
lock of your own. The loop picks the new period up on its next tick.

## 4. See it

Flash, then talk to the device (the `Content-Type` header is the CSRF guard —
without it every write is `415`):

```sh
H='Content-Type: application/json'; D=http://espos-xxxx.local/api/v1
curl -s "$D/config?ns=light"                                    # {"light":{"multiplier":1,"offset":0,"period_ms":500,"enabled":true}}
curl -s -X PUT -H "$H" -d '{"light":{"multiplier":2.5}}' $D/config   # {"changed":["light.multiplier"],"restart_required":false}
curl -s -X PUT -H "$H" -d '{"light":{"period_ms":50}}' $D/config     # 400 {"error":"validation","path":"light.period_ms","message":"out of range [100,60000]"}
curl -s -X PUT -H "$H" -d '{"light":{"multiplier":null}}' $D/config  # back to the default
curl -s "$D/config/schema" | python3 -m json.tool | grep -A3 '"light"'
```

The value in the Data Browser doubles after the second command, without a
reboot. Open `http://espos-xxxx.local/config`: a **Light sensor** section with
a number field per key, the `ms` unit and the ranges as hints, a checkbox for
`enabled` — rendered from the schema, no UI code written. `curl -N $D/events`
shows a `config` event `{"ns":"light","key":"multiplier"}` per change, which
is how the page knows to refresh.

## Growing it later

Adding a key needs no version bump — new keys read their defaults. Renaming,
retyping or changing units bumps `version` and comes with a migration step
([config.md](../config.md), "Migrations"). `"restart_required": true` marks a
key with ↻ in the UI and in the `PUT` response; `"secret": true` keeps a value
out of every export. What a firmware can be configured with is decided at
build time, which is what makes the schema an honest contract.
