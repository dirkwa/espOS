# Examples

Eleven complete ESP-IDF projects, one per thing a device built on espOS
does. Each lives under the component it shows off, builds on its own for
every supported target, and replaces a SensESP example — the table says
which, so a reader coming from SensESP knows where to look.

Levels: **Essential** — read first; the template a new firmware starts from.
**Newbie** — one espOS API in a sensor-sized program. **Advanced** —
threading, boot order, the server's REST tree, TLS.

| Example | Level | What it shows | Replaces (SensESP) | Path |
|---|---|---|---|---|
| `minimal` | Essential | `espos_start(NULL)` and one published value per second | `minimal_app`, `constant_sensor` | `components/espos_core/examples/minimal` |
| `analog_input` | Essential | an ADC reading on a Signal K path | `analog_input` | `components/espos_sk/examples/analog_input` |
| `custom_settings` | Newbie | settings from a JSON descriptor, applied live, one migration | `ConfigItem` | `components/espos_config/examples/custom_settings` |
| `health_and_led` | Newbie | a status LED from the health table; a fatal condition and the watchdog policy | `SystemStatusLed` | `components/espos_health/examples/health_and_led` |
| `pulse_counter` | Newbie | pulses on a pin → frequency / RPM | `rpm_counter`, `pcnt_rpm_counter` | `components/espos_sk/examples/pulse_counter` |
| `digital_switch` | Newbie | a GPIO output switched from Signal K | `smart_switch` | `components/espos_sk/examples/digital_switch` |
| `two_phase_boot` | Advanced | `espos_init()` / `espos_start_network()`, a worker task, a blocking REST lookup | `freertos_tasks` | `components/espos_core/examples/two_phase_boot` |
| `app_endpoint_and_page` | Advanced | REST endpoints, an SSE event, a page in the web UI | frontend plugins | `components/espos_httpd/examples/app_endpoint_and_page` |
| `listener_relay` | Newbie | subscribe to a path, drive a relay | `listener` | `components/espos_sk/examples/listener_relay` |
| `json_and_meta` | Advanced | publish a JSON value, declare metadata for a path of your own | `raw_json`, the metadata example (SensESP #501) | `components/espos_sk/examples/json_and_meta` |
| `tls_server` | Advanced | https/wss to the server (`CONFIG_ESPOS_SK_TLS`) | `ssl_connection` | `components/espos_sk/examples/tls_server` |
| `ble_gateway` | Advanced | a BLE→Signal K gateway: the whole firmware is `espos_start(NULL)`, the configuration is the content | — (SensESP has no equivalent) | `components/espos_ble/examples/ble_gateway` |

## How to build one

An example is an ordinary ESP-IDF project. From its directory, with the IDF
environment exported (`. $IDF_PATH/export.sh`, v6.0.2 — see `.idf-version`):

```sh
cd components/espos_core/examples/minimal
idf.py set-target esp32c6                 # esp32 / esp32s3 / esp32c3 / esp32c6 / esp32p4
idf.py build flash monitor
```

On a shared or small host, `scripts/build.sh` runs the same build under one
machine-wide lock, at half the cores, niced (docs/development.md):

```sh
cd components/espos_core/examples/minimal
../../../../scripts/build.sh -DIDF_TARGET=esp32c6 build      # build/ in the example directory
../../../../scripts/build.sh flash monitor
```

One build directory per target, with its own sdkconfig — what CI and the
multi-target recipe in docs/development.md do:

```sh
../../../../scripts/build.sh -B build-esp32p4 -DSDKCONFIG=build-esp32p4/sdkconfig -DIDF_TARGET=esp32p4 build
```

The first configure warns that it generated a development app-signing key
(`secure_boot_signing_key.pem`, git-ignored): right for a bench, docs/ota.md
before a device leaves it. `idf.py flash` writes the app, the bootloader, the
partition table and the web UI bundle (`storage.bin`).

## What every example has

`CMakeLists.txt` includes the shared prologue (`cmake/espos_project.cmake`):
the IDF version policy, espOS's sdkconfig defaults, the partition table
(4 MB unless the README says otherwise), the components and the signing key
come from there, and nothing of it is copied. `main/CMakeLists.txt` names
the espOS components the example uses — projects on espOS build with IDF's
`MINIMAL_BUILD`, so naming a component is what puts it in the build, and
`espos_start()` starts what is built. `main/idf_component.yml` carries the
IDF pin and the two ESP32-P4 co-processor entries. `main/main.c` is at most
120 lines, comments included, and says why at every step that has a why.

Outside this tree an example is a firmware: copy it, add espOS as the
`espos/` submodule, point the include at `espos/cmake/espos_project.cmake`
(docs/development.md, "Building a firmware on espOS"). `minimal` is the one
to copy.
