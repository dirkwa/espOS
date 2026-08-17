# espOS

A minimal, modern device runtime for SignalK-connected ESP32 hardware, built
natively on ESP-IDF 6.

espOS is the plumbing every SignalK ESP32 device needs and nobody wants to
rewrite: WiFi, persistent config, a web config UI, SignalK server discovery,
token acquisition, delta output, and OTA. It is deliberately **not** a sensor
framework — application code sits on top and calls a small API.

Targets: ESP32, ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4 — one codebase.
Toolchain: ESP-IDF pinned in [`.idf-version`](.idf-version). HTTP:
`esp_http_server`. Storage: NVS for config and secrets.

## Status

| Milestone | Scope                                            | State |
|-----------|--------------------------------------------------|-------|
| M1        | Config store + minimal HTTP server + JSON Schema | ✅ implemented, host-tested |
| M2        | WiFi state machine with reason codes             | ✅ implemented, host-tested, live on ESP32-P4 (BLE provisioning pending) |
| M3        | SignalK discovery + token state machine          | ✅ implemented, host-tested against signalk-server 2.31, live on ESP32-P4 |
| M4        | WebSocket deltas + meta reconciliation           | planned |
| M5        | Web UI (Vite SPA)                                | planned |
| M6        | Signed OTA with rollback                         | planned |

## Decisions and additions vs. the plan

Recorded here so nothing is silently applied (plan §7):

* **ESP-IDF 6.0.2** instead of the plan's original 5.x (owner decision,
  2026-08-18). Consequence: IDF 6 removed the bundled cJSON, so
  `espressif/cjson` (Espressif-maintained, exact-pinned) is the single
  registry dependency.
* Beyond the M1 bullet list, and marked M1 in [docs/api.md](docs/api.md):
  `GET /api/v1/system/info`, `POST /api/v1/system/reboot`, `?ns=` filter on
  `GET /config`, `ETag`/`304` on the schema, `restart_required` in the PUT
  response, and the `Content-Type: application/json` CSRF guard on
  state-changing requests. Each is small and needed by the M1 acceptance
  test or the M5 UI; drop any of them if unwanted before the API is frozen.
* ESP32-P4 pulls `espressif/esp_hosted` + `espressif/esp_wifi_remote`
  (P4-only) because the chip has no radio; approved 2026-08-18.
* M2 ships the SoftAP captive portal; BLE provisioning
  (`espressif/network_provisioning`) is a follow-up, as agreed.
* M3 adds `espressif/mdns` (registry, exact-pinned) for discovery, approved
  2026-08-18. Discovery of `_signalk-ws._tcp` is folded into the
  `_signalk-http._tcp` browse (every server advertises both with the same
  TXT records; the ws endpoint comes from `GET /signalk`).
* Delta buffering during offline periods (listed under M2) lands with the
  delta pipeline in M4 — there is nothing to buffer before that.

## Quick start

```sh
. $IDF_PATH/export.sh            # ESP-IDF v6.0.2, see .idf-version
idf.py set-target esp32c6            # or esp32 / esp32s3 / esp32c3 / esp32p4
idf.py build flash monitor
# join the "espOS-xxxx" access point, the setup page opens; or see docs/wifi.md
curl http://<device-ip>/api/v1/wifi/status
```

Docs: [REST API contract](docs/api.md) · [Config store &
descriptors](docs/config.md) · [WiFi](docs/wifi.md) · [SignalK
discovery & token](docs/signalk.md) · [Development & host
tests](docs/development.md) · [Security notes](docs/security.md)

## Layout

```
components/espos_config/   NVS-backed config store, build-time descriptor → schema/tables
components/espos_httpd/    esp_http_server, /api/v1, SSE, static UI
components/espos_wifi/     station manager + portal (state machine host-testable)
components/espos_sk/       SignalK discovery + access-token state machine
main/                      example app
tools/                     generators
test/host/                 linux-target tests (no hardware needed)
docs/                      contracts and guides
```

## License

Apache-2.0 — see [LICENSE](LICENSE).
