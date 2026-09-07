# Tutorial: your first sensor

**Essential.** From the minimal example to a live reading in the Signal K
Data Browser, with the boot narration and the approval step explained on the
way. The reading is a voltage on an analog pin, the structure of
`components/espos_sk/examples/analog_input`, so a jumper wire is the only sensor. You need ESP-IDF v6.0.2
exported ([getting-started.md](../getting-started.md)), a devkit on USB, and a signalk-server on the LAN with mDNS on, plus its admin login.

## 1. A project of your own

A firmware on espOS is a directory with espOS as a submodule and the shared prologue
included from it ([development.md](../development.md)). The minimal example boots to the web UI as it is, so start from its `main/`:

```sh
mkdir first-sensor && cd first-sensor && git init
git submodule add https://github.com/signalk-espOS/espOS espos
cp -r espos/components/espos_core/examples/minimal/main .
```

The example's `CMakeLists.txt` reaches the prologue by a relative path inside
the espOS tree; yours reaches it through the submodule — five lines:

```cmake
cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/espos/cmake/espos_project.cmake")
espos_project_prologue(NAME "first-sensor")
project(first_sensor)
espos_project_ui_partition()
```

Build, flash, watch (`-p` is your serial port; the target is any of `esp32 esp32s3 esp32c3 esp32c6 esp32p4`).
The first configure warns that it generated a development signing key: expected, and OTA-only ([ota.md](../ota.md)).

```sh
espos/scripts/build.sh -DIDF_TARGET=esp32c6 build
espos/scripts/build.sh -p /dev/ttyUSB0 flash monitor
```

## 2. Reading the boot

The device narrates itself on the monitor, one line per milestone, in the
order `espos_start()` runs ([concepts.md](../concepts.md)):

* `espOS 0.7.0 on esp32c6 — app first-sensor …` — log ring and config store up; espOS's version, then yours.
* `no network configured: join "espOS-1a2b" and open http://192.168.4.1` — the portal is up; nothing else happens until a network is known.
* `connected to "Boat" as 192.168.1.23 — web UI: http://espos-1a2b.local` — station up; the web UI answers at that name.
* `found signalk-server "boat" at 192.168.1.10:3000 (discovered …)` — mDNS found a server; nothing sent yet.
* `access requested — approve it in the server UI: Security → Access Requests` — the device asked for a token and polls for the answer.
* `approved, streaming` — token stored; the delta stream is open.

Do what the second line says: join the `espOS-xxxx` access point from a
phone; the setup page opens (or browse to `http://192.168.4.1`); pick your
WiFi, enter the password. The portal drops the moment the station connects.
`http://espos-xxxx.local/wifi` shows what it wrote; `wifi.hostname` renames the device.

## 3. The approval step

A server with security enabled takes no data from a stranger: the device asks
for access under a name (`first-sensor espos-1a2b`) and a client id it keeps
for life, and an administrator approves once. In the server UI open
**Security → Access Requests**, choose read/write and an expiry of never,
approve. The monitor says `approved, streaming`; `http://espos-xxxx.local/signalk`
shows the token state `approved`. The token is stored in NVS keyed to that
server, so a reboot, a config export or a server that changes IP address does not
repeat this step; a reinstalled server does ([signalk.md](../signalk.md)). Denied →
`denied`, no automatic retry, "request again" on the SignalK page; security off →
`open`, data flows without a token. Health values (`espos.espos-1a2b.uptime`, …) already arrive every 10 s.

## 4. The reading

Replace `main/main.c` with a loop that reads one ADC channel and publishes it
as a ratio; add `esp_adc` to `PRIV_REQUIRES` in `main/CMakeLists.txt` (`espos_sk` is already there).

```c
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "espos.h"
#include "espos_sk.h"
#define ADC_CH ADC_CHANNEL_4 /* ADC1 channel 4: GPIO4 on ESP32-C6, GPIO20 on ESP32-P4 — the analog_input example's pins */
#define PATH   "environment.indoor.illuminance"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL));
    espos_sk_declare_meta(PATH, "{\"units\":\"ratio\",\"description\":\"Light on the bench\"}", 500); /* not a spec path */
    adc_oneshot_unit_handle_t adc;
    adc_cali_handle_t cali; /* eFuse calibration, raw counts → millivolts; the original ESP32 uses _line_fitting */
    adc_oneshot_unit_init_cfg_t unit = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_chan_cfg_t chan = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    adc_cali_curve_fitting_config_t cc = { .unit_id = ADC_UNIT_1, .chan = ADC_CH, .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit, &adc));
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CH, &chan));
    ESP_ERROR_CHECK(adc_cali_create_scheme_curve_fitting(&cc, &cali));
    for (;; vTaskDelay(pdMS_TO_TICKS(500))) {
        int raw, mv;
        if (adc_oneshot_read(adc, ADC_CH, &raw) == ESP_OK && adc_cali_raw_to_voltage(cali, raw, &mv) == ESP_OK) {
            espos_sk_publish_number(PATH, mv / 3300.0); /* 0 V → 0, 3.3 V → 1 */
        }
    }
}
```

`espos_sk_publish_number()` is the whole Signal K side of a sensor: thread-safe,
never blocks, batched per `sk.batch_ms`, buffered while the stream is down. This
path is not in the specification, hence the meta declaration; a spec path needs none ([tank-level](tank-level.md)).

## 5. See it

Build and flash again. In the Data Browser filter `illuminance`:
`environment.indoor.illuminance` from source `espos.espos-1a2b`, twice a second.
Jumper the pin to GND and the value drops to 0; to 3V3 and it rises to about 1 (the ADC saturates just below the rail).

```sh
curl -s http://<server>:3000/signalk/v1/api/vessels/self/environment/indoor/illuminance
curl -s http://espos-xxxx.local/api/v1/sk/status | python3 -m json.tool   # ws.sent climbs
```

Next: [add-a-setting](add-a-setting.md) makes the scale factor editable in the UI; [tank-level](tank-level.md) turns the same pin into a tank gauge.
