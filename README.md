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

espOS lives at [github.com/signalk-espOS/espOS](https://github.com/signalk-espOS/espOS)
and publishes its components as `signalk-espos/espos_*`. It is a community
project, not an official Signal K repository.

## Status

espOS is at **v0.7.0**. The core is done and in use: config store, HTTP
server and REST API, WiFi state machine with captive portal, SignalK
discovery, access token and delta stream in both directions, web UI,
device-health notifications and signed OTA with rollback -- all host-tested
and running on ESP32-P4 hardware against signalk-server 2.31; the BLE,
NMEA 2000 and voice components serve firmware built on top. What comes next
is in [docs/roadmap.md](docs/roadmap.md); the decisions taken so far are in
[docs/decisions.md](docs/decisions.md).

## Quick start

```sh
. $IDF_PATH/export.sh                # ESP-IDF v6.0.2, see .idf-version
idf.py set-target esp32c6            # or esp32 / esp32s3 / esp32c3 / esp32p4
idf.py build flash monitor           # on a shared or small host: scripts/build.sh build
# the monitor says what to do next: join the "espOS-xxxx" access point and open
# http://192.168.4.1 to pick a WiFi; then approve the device in signalk-server
# (Security → Access Requests). Web UI afterwards: http://<hostname>.local
```

The web UI bundle is committed, so Node is not needed to build a device that
serves it. `scripts/build.sh` wraps `idf.py` with a machine-wide lock and a
capped job count for hosts that freeze under a full parallel build.

The whole of an application on espOS is one call; everything else is yours
([docs/concepts.md](docs/concepts.md) has the order and the threading rules):

```c
#include "espos.h"

void app_main(void)
{
    ESP_ERROR_CHECK(espos_start(NULL)); /* log → config → httpd → wifi → sk → ota → ble */
    /* your application: espos_sk_publish_*, espos_sk_subscribe, espos_config_get_* */
}
```

Docs: [REST API contract](docs/rest-api.md) · [Config store &
descriptors](docs/config.md) · [WiFi](docs/wifi.md) · [SignalK
discovery & token](docs/signalk.md) · [OTA & signing](docs/ota.md) ·
[Web UI](docs/ui.md) · [Device health](docs/health.md) · [BLE gateway](docs/ble.md) · [NMEA 2000
gateway](docs/n2k.md) · [Voice satellite](docs/voice.md) ·
[Development & host tests](docs/development.md) · [Releasing](docs/releasing.md) · [Security
notes](docs/security.md)

## Components

Everything is an ESP-IDF component; an application depends on the ones it
needs and ignores the rest. The core four are what "running espOS" means;
the rest are optional.

| Component | What it gives you | Docs |
|---|---|---|
| `espos_config` | NVS config store, JSON-Schema descriptors, REST-backed settings | [config.md](docs/config.md) |
| `espos_httpd` | HTTP server, REST API, SSE, the web UI from a LittleFS partition | [rest-api.md](docs/rest-api.md) · [ui.md](docs/ui.md) |
| `espos_wifi` | Station + provisioning portal, a pure-C state machine, co-processor watchdog, mDNS responder | [wifi.md](docs/wifi.md) |
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

espOS is open source under the [Apache License 2.0](LICENSE); every source
file carries an SPDX header naming the license and the tree is
[REUSE](https://reuse.software)-compliant. Contributions are accepted under the
same license with a `Signed-off-by` line (Developer Certificate of Origin).

Third-party components pulled in by the build (ESP-IDF, Espressif component
registry packages, `joltwallet/littlefs`, npm packages) keep their own
licenses; see [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
