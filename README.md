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
| M4        | WebSocket deltas + meta reconciliation           | ✅ implemented, host-tested (mock stream), live on ESP32-P4 against signalk-server 2.31 |
| M5        | Web UI (Vite SPA) + logs + core dump             | ✅ implemented, developed against a mock, verified served from LittleFS on ESP32-P4 |
| M6        | Signed OTA with rollback, manifest               | ✅ implemented, host-tested (sim port), verified on ESP32-P4: manifest → install → confirm; unsigned image rejected; broken image rolled back by the bootloader |
| M7        | SignalK inbound: subscriptions, meta, PUT        | ✅ implemented (added for the cockpit port, not in the original plan), host-tested, verified against signalk-server 2.31 on ESP32-P4 |

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
* Delta buffering during offline periods (listed under M2) landed with the
  delta pipeline in M4 — there was nothing to buffer before that.
* M5 adds `joltwallet/littlefs` (registry, exact-pinned) — the plan names
  LittleFS for the UI bundle and this is the ESP-IDF component for it — and
  the UI's build-time npm dependencies (`preact`, `vite`,
  `@preact/preset-vite`, `typescript`; nothing at runtime but preact).
  Beyond the M5 bullets: `PUT /api/v1/logs/level` (runtime log level) and
  `GET /api/v1/system/coredump/raw` (download for `espcoredump.py`), both
  small and needed to make logs/crashes actually useful from a browser.
* M6 signature verification is *signed apps without Secure Boot*
  (`SECURE_SIGNED_APPS_NO_SECURE_BOOT`, RSA-3072, verified on update by the
  running app's compiled-in public key) rather than hardware Secure Boot:
  it is what the plan asks for ("against a compiled-in public key") without
  burning eFuses. The signing key is generated on first build when missing
  (git-ignored) so a checkout builds; real deployments bring their own
  (docs/ota.md). Plain-http image sources are allowed for the same reason.
  ESP32 (original) builds pin chip rev ≥ 3 for the RSA scheme.

## Quick start

```sh
. $IDF_PATH/export.sh            # ESP-IDF v6.0.2, see .idf-version
idf.py set-target esp32c6            # or esp32 / esp32s3 / esp32c3 / esp32p4
(cd ui && npm ci && npm run build)   # optional: the web UI → LittleFS image
idf.py build flash monitor
# join the "espOS-xxxx" access point, the setup page opens; or see docs/wifi.md
curl http://<device-ip>/api/v1/wifi/status
```

Docs: [REST API contract](docs/api.md) · [Config store &
descriptors](docs/config.md) · [WiFi](docs/wifi.md) · [SignalK
discovery & token](docs/signalk.md) · [OTA & signing](docs/ota.md) ·
[Web UI](docs/ui.md) · [Device health](docs/health.md) · [BLE gateway](docs/ble.md) · [NMEA 2000
gateway](docs/n2k.md) · [Voice satellite](docs/voice.md) ·
[Development & host tests](docs/development.md) · [Security
notes](docs/security.md)

## Components

Everything is an ESP-IDF component; an application depends on the ones it
needs and ignores the rest. The core four are what "running espOS" means;
the rest are optional.

| Component | What it gives you | Docs |
|---|---|---|
| `espos_config` | NVS config store, JSON-Schema descriptors, REST-backed settings | [config.md](docs/config.md) |
| `espos_httpd` | HTTP server, REST API, SSE, the web UI from a LittleFS partition | [api.md](docs/api.md) · [ui.md](docs/ui.md) |
| `espos_wifi` | Station + provisioning portal, a pure-C state machine, co-processor watchdog | [wifi.md](docs/wifi.md) |
| `espos_log` | Log ring served over REST, so a device is debuggable without a serial cable | — |
| `espos_health` | Device conditions (warn/alarm) and the sinks that consume them | [health.md](docs/health.md) |
| `espos_sk` | SignalK: mDNS discovery, access token, delta stream in and out | [signalk.md](docs/signalk.md) |
| `espos_ota` | Signed OTA with rollback, from a URL or a version manifest | [ota.md](docs/ota.md) |
| `espos_ble` | BLE gateway | [ble.md](docs/ble.md) |
| `espos_n2k` | NMEA 2000 over TWAI + a candump TCP server | [n2k.md](docs/n2k.md) |
| `espos_audio` | The `AudioDriver` contract a board implements (header-only) | [voice.md](docs/voice.md) |
| `espos_voice` | Wyoming voice satellite with esp-sr wake word | [voice.md](docs/voice.md) |

Board-specific code — display HALs, audio codecs, pin maps — stays in the
application. espOS defines the contracts and never assumes a particular
board; `espos_audio::AudioDriver` is the pattern to copy when something
similar is needed.

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

espOS is **source available, not open source**. See [LICENSE.md](LICENSE.md)
(SPDX headers use `LicenseRef-Source-Available-No-Redistribution`).

**You may**, free of charge: run it on your own boat or fleet, private or
commercial; use it for internal company operations; modify it for your own
use; use it in education and research; and provide professional services
around it.

**You may not**: redistribute it, or publish a modified version of it — as
source, firmware images or otherwise. Verbatim copies of official releases
may be mirrored and cached.

Third-party components pulled in by the build (ESP-IDF, the Espressif
component registry, `joltwallet/littlefs`, npm packages) keep their own
licenses.
