# Examples

Every example is a complete ESP-IDF project of its own under
`components/<component>/examples/<name>/`, built the same way as any firmware
on espOS: it includes `cmake/espos_project.cmake`, calls
`espos_project_prologue()` and `espos_project_ui_partition()`, and its
`main.c` boots with `espos_start()`. What each one adds on top is the point
of the example and fits in about a hundred lines. Each README says what the
example does, what it needs wired, what appears in Signal K, and — for
readers coming from SensESP — which SensESP example it replaces, and carries
a label: **Essential** (read these first), **Newbie** (one concept, no
surprises) or **Advanced**.

The index in the repository is
[`examples/README.md`](https://github.com/signalk-espOS/espOS/blob/main/examples/README.md);
the table below is the same list with the page it pairs with on this site.

## Build any of them

```sh
cd components/espos_sk/examples/analog_input     # or any other example directory
idf.py set-target esp32c6                        # esp32 / esp32s3 / esp32c3 / esp32c6 / esp32p4
idf.py build flash monitor
```

Every example builds for `esp32c6`; the ones that drive a peripheral also
build for `esp32p4`, with the pins as `#define`s at the top of `main.c`. On
a small or shared machine use the locked wrapper instead of a bare build:
`/path/to/espOS/scripts/build.sh build` from inside the example directory
([Development](development.md)). The first configure generates a
*development* signing key with a warning; that is expected
([OTA → Signing key](ota.md#signing-key)).

## The examples

| Example | Component | What it shows | Read with |
|---|---|---|---|
| [`minimal`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/minimal) | `espos_core` | **Essential.** The whole of an espOS application: `espos_start(NULL)`, then `environment.inside.temperature` once a second, a constant (293.65 K) standing in for the sensor. No wiring. Replaces SensESP's `minimal_app` and `constant_sensor`. The [getting started](getting-started.md) target. | [Concepts](concepts.md) |
| [`two_phase_boot`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_core/examples/two_phase_boot) | `espos_core` | **Advanced.** `espos_init()` and `espos_start_network()` instead of one call, with the application's own work between them, and the pattern for a blocking espOS call — a worker task fed through a queue. Replaces `freertos_tasks`. | [Concepts → espos_start()](concepts.md#espos_start-the-order-and-why), [Threading contracts](concepts.md#threading-contracts) |
| [`custom_settings`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_config/examples/custom_settings) | `espos_config` | **Newbie.** An application's own settings: declared once in `main/config/app.json`, used through generated key constants, applied live from the web UI, carried across a renamed key by a migration. Replaces SensESP's `ConfigItem`. | [Configuration store](config.md), [Add a setting](tutorials/add-a-setting.md) |
| [`app_endpoint_and_page`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_httpd/examples/app_endpoint_and_page) | `espos_httpd` | **Advanced.** A firmware's own REST endpoints on the espOS server, a live value on the SSE stream the web UI already listens to, and a page of your own in that UI. Replaces SensESP's frontend plugins. | [REST API](rest-api.md), [Web UI](ui.md), [An app endpoint and a UI tab](tutorials/app-endpoint-and-ui-tab.md) |
| [`health_and_led`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_health/examples/health_and_led) | `espos_health` | **Newbie.** A status LED driven from the device's health table, and an application condition the watchdog policy acts on — and what it never restarts for. Replaces SensESP's `SystemStatusLed`. | [Device health](health.md) |
| [`analog_input`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/analog_input) | `espos_sk` | **Essential.** One ADC pin read every second — factory-calibrated, averaged, scaled by two settings from the web UI — published to a Signal K path: the shape of every "sensor on a wire" device. Replaces SensESP's `analog_input`, `repeat_sensor_analog_input` and its tank-level tutorial. | [Your first sensor](tutorials/first-sensor.md), [Tank level](tutorials/tank-level.md) |
| [`pulse_counter`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/pulse_counter) | `espos_sk` | **Newbie.** Pulses on a GPIO counted by the PCNT peripheral with its glitch filter, published as revolutions per second, with a running total kept in the config store across reboots — engine RPM, shaft speed, chain or flow. Replaces `rpm_counter`, `pcnt_rpm_counter`, `chain_counter`, `time_counter`. | [Signal K → Delta stream](signalk.md#delta-stream-m4) |
| [`digital_switch`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/digital_switch) | `espos_sk` | **Newbie.** A GPIO output whose state is a Signal K switch path, toggled by a debounced button; published on change and every 10 s so a late dashboard still shows the truth. Replaces SensESP's `smart_switch`. | [Signal K → Inbound](signalk.md#inbound-m7) |
| [`listener_relay`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/listener_relay) | `espos_sk` | **Newbie.** A relay driven by a value the server streams: subscribe to `environment.outside.illuminance`, switch on below 50 lux and off above 100 (hysteresis), publish the relay's state back. Replaces SensESP's `listener`. | [Concepts → Threading contracts](concepts.md#threading-contracts) |
| [`json_and_meta`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/json_and_meta) | `espos_sk` | **Advanced.** The three calls a plain number does not cover, framed as a windlass controller: `espos_sk_publish_json()` for an object, `espos_sk_declare_meta()` for a path only this device knows, `espos_sk_notify()` for a condition of the device. No hardware. Replaces `raw_json` and the `metadata` example. | [Signal K → Delta stream](signalk.md#delta-stream-m4), [Device health](health.md) |
| [`tls_server`](https://github.com/signalk-espOS/espOS/tree/main/components/espos_sk/examples/tls_server) | `espos_sk` | **Advanced.** The Signal K connection over https and wss: `CONFIG_ESPOS_SK_TLS` in the build, `sk.tls` in the configuration, no change to application code, plus one diagnostic GET that says whether the certificate verified. Replaces SensESP's `ssl_connection`. | [Signal K → TLS](signalk.md#tls-https-wss), [Security](security.md) |

The reference application in `main/` at the repository root is not an
example but the app espOS's own CI builds on every target; it exercises every
descriptor type and every optional component, which is why it is larger than
any example.

## Where to go from an example

* Something in the runtime behaves unexpectedly: [Troubleshooting](troubleshooting.md).
* The example does nearly what you want: the [tutorials](tutorials/first-sensor.md)
  walk from an example to a device of your own, one addition at a time.
* You have a SensESP project: [Migrating from SensESP](migration-from-sensesp.md)
  maps its concepts onto these examples.
