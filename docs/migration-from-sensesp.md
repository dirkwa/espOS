# Migrating from SensESP

For people who have a SensESP project — PlatformIO, the Arduino core, a
`SensESPAppBuilder`, a `connect_to()` chain ending in an `SKOutputFloat` —
and want the same device on espOS. What you know carries over: Signal K paths
and SI units, metadata for non-standard paths, the approval step on the
server, the shape of every sensor program (read, calibrate, publish). What
changes is below. [concepts.md](concepts.md) is the map of what you are moving
onto; the tutorials start with [first-sensor](tutorials/first-sensor.md).

## The three things that change

### 1. Toolchain: PlatformIO and Arduino → ESP-IDF 6 and `idf.py`

SensESP builds with PlatformIO on the Arduino-ESP32 core. espOS is native
ESP-IDF 6.0.x (`.idf-version`, currently v6.0.2) with no Arduino layer: CMake
projects, Kconfig, `idf.py` (or `scripts/build.sh`, which wraps it) and the
IDF drivers — `esp_adc` instead of `analogReadMilliVolts()`, `esp_driver_gpio`
instead of `digitalRead()`, `i2c_master` instead of `Wire`, `esp_driver_pcnt`
instead of an interrupt counter. `String` and ArduinoJson are gone; the espOS
API is plain C with `esp_err_t` returns, cJSON is there when you need JSON.
Your own code may be C++ (`espos_n2k` and `espos_voice` are), but there is no
`setup()`/`loop()`: the entry point is `app_main()`, and a task that wants to
run forever loops itself. A project is a five-line root `CMakeLists.txt` plus
`main/` ([development.md](development.md), "Building a firmware on espOS");
`platformio.ini` has no counterpart — target, flash and partitions come from the prologue, pins are `#define`s in your code.

### 2. Model: a producer/consumer graph → plain C calls, today

In SensESP, objects are created in `setup()` and wired with `connect_to()`;
the event loop ticks them and values flow along the graph. espOS has no graph
today. The runtime gives you the plumbing (WiFi, config, HTTP, the Signal K
token and the delta stream) and a small C API; your application is a task or
a timer that reads the sensor, does its arithmetic in C and calls
`espos_sk_publish_number()`. Batching, offline buffering, reconnects and
metadata reconciliation happen behind that call ([signalk.md](signalk.md)).

Honestly stated: a typed C++ facade — producer/consumer/transform nodes,
`Poll<T>`, `Linear`, `MovingAverage`, `sk::Output<T>`, chained with
`connect_to()` or `>>` — is planned as a later tranche (`espos_flow`). It does
not exist yet; the names in the "planned" column below come from the plan and
may change. It will be header-only sugar over the C API described here, never
a replacement for it: the C shape you write today is what it will produce.

### 3. Config: `ConfigItem` per object → one JSON descriptor per namespace

In SensESP every configurable object has a `config_path`, serialises itself
(`to_json`/`from_json`), carries a schema per class, and `ConfigItem()` puts
a card for it in the web UI. In espOS a setting exists in exactly one place: a
JSON descriptor per NVS namespace, registered from `CMakeLists.txt` with
`espos_config_add_descriptor(config/<ns>.json)`. The build generates the key
constants (`ESPOS_CFG_<NS>_<KEY>`), the validation tables and the JSON Schema;
the web UI renders the form from the schema; `PUT /api/v1/config` validates a
whole document before writing any of it; descriptor versions drive migrations
([config.md](config.md)). Code reads a setting with `espos_config_get_*()` and
follows changes with `espos_config_subscribe()`. Title and description live in
the descriptor, `restart_required` replaces `set_requires_restart()`, there is
no sort order (namespaces appear in name order). Settings are declared, not
owned by objects — which is why one survives a rewrite of the code using it.

## Symbol by symbol

| SensESP | espOS today | Planned facade |
|---|---|---|
| `SensESPAppBuilder` … `get_app()` | `espos_start(NULL)` (`espos.h`); options in `espos_start_opts_t` (`app_name`, `before_network`, `health_watchdog`) | same call underneath |
| `set_hostname("x")` | the `wifi.hostname` setting (portal, UI, `PUT /api/v1/config`); default `espos-<id>`. `espos_start_opts_t.app_name` names the device in the server's access-request list | same |
| `set_wifi_client(ssid, psk)` | never in code: the portal, or `wifi.ssid0`/`psk0` flashed as an NVS image ([wifi.md](wifi.md)) | same |
| `set_sk_server(host, port)` | `sk.server_host`/`sk.server_port`; unset = mDNS discovery, sticky to the server the token belongs to | same |
| `SetupLogging()` | nothing to call: `espos_start()` installs the log ring; level via `CONFIG_LOG_DEFAULT_LEVEL_*`, at runtime `PUT /api/v1/logs/level` | same |
| `event_loop()->onRepeat()` / `onDelay()` / `onInterrupt()` | a FreeRTOS task with `vTaskDelay()` or an `esp_timer` / `esp_timer_start_once()` / `gpio_isr_handler_add()` posting to a queue your task reads | `espos_flow_every()`, `Poll<T>` / `espos_flow_after()` / `GpioChange` |
| `RepeatSensor<T>(ms, lambda)` | the loop in `components/espos_sk/examples/analog_input`: read, compute, `espos_sk_publish_*()` | `Poll<T>` |
| `AnalogInput` | `esp_adc` oneshot + eFuse calibration (`analog_input` example, worked below) | `sensors::Analog` |
| `DigitalInputCounter` / `Change` / `State` | `esp_driver_pcnt` (`components/espos_sk/examples/pulse_counter`) / GPIO interrupt + queue / `gpio_get_level()` in the loop (`components/espos_sk/examples/digital_switch`) | `PulseCounter` / `GpioChange` / `GpioState` |
| `DigitalOutput` | `gpio_set_level()` (`components/espos_sk/examples/listener_relay`) | `GpioOutput` |
| `Linear` / `MovingAverage` / `Frequency` | `v * m + b` with `m`, `b` as descriptor keys (`analog_input` example) / a ring of `n` floats and a running sum / count ÷ elapsed `esp_timer_get_time()` (`pulse_counter` example) | `Linear` / `MovingAverage` / `Frequency` |
| `Hysteresis` / `FloatThreshold` / `LambdaTransform` | an `if` with two thresholds and a remembered state / a comparison / a C function | `Hysteresis` / `Threshold` / `Lambda` |
| `SKOutputFloat`, `SKOutputInt` / `SKOutputBool` / `SKOutputString` / `SKOutputRawJson` | `espos_sk_publish_number()` / `_bool()` / `_string()` / `_json()` — thread-safe, never block, work before WiFi is up | `sk::Output<T>` |
| `SKMetadata` | `espos_sk_declare_meta(path, meta_json, period_ms)` — non-standard paths only; reconciled on every connect, server edits win (`components/espos_sk/examples/json_and_meta`) | `sk::Meta` |
| `SKValueListener<T>` | `espos_sk_subscribe(pattern, period_ms, cb, arg)`; families (`navigation.*`) too (`listener_relay` example) | `sk::Listener<T>` |
| `SKPutRequest<T>` | `espos_sk_put(path, value_json, cb, arg)` | `sk::PutRequest<T>` |
| `SKPutRequestListener<T>` | **not yet** — no inbound PUT handler; a server PUT to the device's path is answered `405` by the server | `sk::PutHandler<T>` |
| `ConfigItem(obj)->set_title()->set_description()->set_requires_restart()` | one key in the namespace's descriptor: `title`, `description`, `restart_required`; read with `espos_config_get_*()`, follow with `espos_config_subscribe()` (`components/espos_config/examples/custom_settings`: string, enum, a version-2 migration) | `Param<T>` |
| `SystemStatusLed` | an `espos_health_add_sink()` sink plus `ESPOS_EVENT` handlers driving a GPIO (`components/espos_health/examples/health_and_led`) | same |
| `SensESPMinimalAppBuilder` | `espos_init()` without `espos_start_network()` (`components/espos_core/examples/two_phase_boot`), or a project that does not require `espos_sk` | same |
| `StatusPageItem` / `UIButton` | your own `GET`/`POST` endpoints via `espos_httpd_register()`, `espos_httpd_sse_publish()`, and a `registerPage()` tab (`components/espos_httpd/examples/app_endpoint_and_page`, [ui.md](ui.md)) | a generic Flow page |
| `enable_system_info_sensors()` | built in: `espos.<hostname>.{uptime,freeHeap,internalFree,rssi,…}` every `sk.health_s` | same |
| `enable_ota(password)` | `espos_ota`: signed images, rollback, a version manifest ([ota.md](ota.md)); the signature is the credential | same |
| `enable_wifi_watchdog()` | `espos_health`'s policy: a lost link is a `WARN` that never restarts; a stalled Signal K link over a link that claims to be up does ([health.md](health.md)) | same |

## A worked migration: `examples/analog_input.cpp`

SensESP's example reads a light sensor on an analog pin every 500 ms,
calibrates it with a `Linear` the user can adjust in the UI, and sends the
result as a ratio on `environment.indoor.illuminance` with metadata. Stripped
of comments:

```c++
#include "sensesp/sensors/sensor.h"
#include "sensesp/signalk/signalk_output.h"
#include "sensesp/transforms/linear.h"
#include "sensesp_app_builder.h"
using namespace sensesp;

void setup() {
  SetupLogging();
  SensESPAppBuilder builder;
  sensesp_app = builder.get_app();
  auto* analog_input = new RepeatSensor<float>(500, []() {
    return analogReadMilliVolts(34) / 1000.;
  });
  auto* cal = new Linear(1.7007, -0.1650, "/indoor_illuminance/linear");
  ConfigItem(cal)->set_title("Input Calibration")->set_description("Analog input value adjustment.");
  analog_input->connect_to(cal)->connect_to(new SKOutputFloat(
      "environment.indoor.illuminance", "", new SKMetadata("ratio", "Indoor light")));
}
void loop() { event_loop()->tick(); }
```

The espOS version is `components/espos_sk/examples/analog_input`, condensed
here to what corresponds (the example adds the calibration-scheme `#if` for the
original ESP32, 16-sample averaging and a log line). The `Linear`'s parameters
become a descriptor, `main/config/app.json` — the example ships `1.0`/`0.0`; a port carries SensESP's constants:

```json
{"namespace": "app", "version": 1, "title": "Analog input",
 "keys": [
  {"name": "multiplier", "type": "float", "default": 1.7007, "title": "Multiplier"},
  {"name": "offset", "type": "float", "default": -0.165, "title": "Offset", "description": "Analog input value adjustment."},
  {"name": "period_ms", "type": "int", "default": 500, "min": 100, "max": 60000, "unit": "ms", "title": "Sample period"}]}
```

registered in `main/CMakeLists.txt` next to the IDF ADC driver:

```cmake
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_config espos_sk esp_adc)
espos_config_add_descriptor(config/app.json)
```

and `main/main.c` is the whole program — `RepeatSensor` is the loop, `Linear`
is one multiply-add, `SKOutputFloat` is one publish call:

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "espos.h"
#include "espos_cfg_keys.h"
#include "espos_config.h"
#include "espos_sk.h"

#define ADC_GPIO 4 /* ADC1 on the ESP32-C6 DevKitC (GPIO 0..6); the example uses 20 on the ESP32-P4 */
#define SK_PATH  "environment.inside.illuminance" /* a spec path: the server owns its meta */

static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_chan;
static float s_multiplier = 1.7007f, s_offset = -0.165f; /* word-sized: written on the HTTP task, read by the loop */
static int32_t s_period_ms = 500;

/* Initial load and the change callback (runs on the writer's task, usually an HTTP handler). */
static void load_cfg(const char *ns, const char *key, void *arg)
{
    (void)ns; (void)key; (void)arg;
    espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_MULTIPLIER, &s_multiplier);
    espos_config_get_float(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_OFFSET, &s_offset);
    espos_config_get_i32(ESPOS_CFG_NS_APP, ESPOS_CFG_APP_PERIOD_MS, &s_period_ms);
}

static void adc_init(void)
{
    adc_unit_t unit;
    ESP_ERROR_CHECK(adc_oneshot_io_to_channel(ADC_GPIO, &unit, &s_chan));
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = unit };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &s_adc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(s_adc, s_chan, &ccfg));
    adc_cali_curve_fitting_config_t cal = { .unit_id = unit, .chan = s_chan, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cal, &s_cali)); /* eFuse data: what analogReadMilliVolts() used */
}

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));                 /* WiFi, portal, token, stream, UI, OTA */
    load_cfg(NULL, NULL, NULL);
    ESP_ERROR_CHECK(espos_config_subscribe(load_cfg, NULL));
    adc_init();
    for (;; vTaskDelay(pdMS_TO_TICKS(s_period_ms))) {   /* RepeatSensor<float>(500, …) */
        int raw, mv;
        ESP_ERROR_CHECK(adc_oneshot_read(s_adc, s_chan, &raw));
        if (adc_cali_raw_to_voltage(s_cali, raw, &mv) == ESP_OK) {
            espos_sk_publish_number(SK_PATH, mv / 1000.0f * s_multiplier + s_offset); /* Linear → SKOutputFloat */
        }
    }
}
```

One deliberate change: SensESP's `environment.indoor.illuminance` is not a
specification path, which is why its example had to send `SKMetadata("ratio", …)`;
the espOS example publishes the spec path `environment.inside.illuminance`, whose
units the server knows. To keep a path of your own, `SKMetadata` is one line before the loop:
`espos_sk_declare_meta(SK_PATH, "{\"units\":\"ratio\",\"displayName\":\"Indoor light\"}", s_period_ms);`.
WiFi, hostname and server are configuration, entered once through the portal or
the UI; the calibration is edited on the Config page (or `PUT /api/v1/config
{"app":{"multiplier":2}}`) and reaches the loop through `load_cfg`. Nothing here knows about WebSockets, tokens or reconnects.

## What has no equivalent yet

* **Arduino driver libraries** (Adafruit, SparkFun, …) do not compile on
  IDF. Check the Espressif component registry first (`idf.py add-dependency
  "<ns>/<name>"`), then port: an I²C breakout is a few register reads. With
  IDF 6's `i2c_master`: `i2c_new_master_bus()` once (`sda_io_num`, `scl_io_num`,
  `clk_source = I2C_CLK_SRC_DEFAULT`), `i2c_master_bus_add_device()` per address,
  `i2c_master_transmit_receive()` to write a register address and read bytes back,
  `i2c_master_transmit()` to write. Translate the library's `readRegister()`/`writeRegister()`
  pair, keep its formulas, drop the rest. Run the bus from your own task, never from a callback espOS calls you on.
* **A permanent access point.** SensESP keeps its AP up next to the station.
  espOS brings the portal up only while provisioning (no network configured,
  or retrying for `wifi.portal_after_s`) and drops it once the station
  connects ([wifi.md](wifi.md)).
* **Ethernet.** SensESP has an ESP32-P4 Ethernet provisioner; espOS is WiFi
  only today (`espos_net` with a wired interface is on the roadmap).
* **PUT handlers** (`SKPutRequestListener`): the device cannot yet accept a
  PUT from the server; until it can, a remote switch subscribes to its own path.
* **The transforms library** (`CurveInterpolator`, `Median`, `Debounce`, `Join`/`Zip`,
  `Throttle`, …): write the arithmetic in C for now; the facade brings them back as nodes.
* **Web UI login** (`set_admin_user`): the REST API and the UI have no authentication today ([rest-api.md](rest-api.md)).

## What espOS has that SensESP does not

* **Signed OTA with rollback and a manifest.** Every image is RSA-signed and
  verified on the device; a new image must confirm itself (WiFi up) or the
  bootloader boots the previous slot; a static `manifest.json` on any web
  server is a fleet update channel ([ota.md](ota.md),
  [tutorial](tutorials/ota-from-a-manifest.md)).
* **A token state machine** that survives reality: a persistent `clientId`, the
  token keyed by the server's `self` so a server that changes address keeps working,
  a pending request that resumes after a reboot, manual paste, re-request from `denied` ([signalk.md](signalk.md)).
* **Reason-coded WiFi**: `reason: {code, text}` for every failure ("wrong password",
  "network not in range", "associated but no IP"), four networks in priority order,
  BSSID pinning, exponential backoff, a co-processor link watchdog on the ESP32-P4 ([wifi.md](wifi.md)).
* **A health policy**: level-triggered conditions with sinks, `lowMemory` measured
  on internal RAM, watched tasks, a strike-counting watchdog that never restarts on
  a lost network, a reset record the next boot can read ([health.md](health.md)).
* **Host tests**: state machines, parsers and the REST server run on the linux target under Unity and a Python harness ([development.md](development.md)).
* **Five targets from one tree**: ESP32, S3, C3, C6 and P4 (WiFi over an
  ESP32-C6 co-processor), same code, same `sdkconfig.d/` defaults.
* Smaller things you will miss going back: the log ring and core dump over
  REST ([tutorial](tutorials/logs-and-core-dumps.md)), SSE instead of polling,
  validated config export/import, descriptor migrations, `_espos._tcp` records.
