# Getting started

From a fresh clone to a device that streams a value into Signal K. Most of
the wall-clock time is the toolchain install and the first build; the espOS
part is a handful of commands and two clicks in the server's UI.

## Prerequisites

* **ESP-IDF 6.0.x.** espOS is tested on the release named in `.idf-version`
  (v6.0.2); any other 6.0.x builds with one warning, and anything outside
  6.0 is refused ([Troubleshooting](troubleshooting.md#build)). Install it
  with the [Espressif Installation Manager](https://docs.espressif.com/projects/idf-im-ui/en/latest/)
  or the [classic guide](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32c6/get-started/index.html),
  then export it in every shell you build from — the installer prints the
  exact line, which is a form of

  ```sh
  . $IDF_PATH/export.sh
  ```

* **A board** with an ESP32, ESP32-S3, ESP32-C3, ESP32-C6 or ESP32-P4
  ([Hardware](hardware.md)). The commands below say `esp32c6`; substitute
  your chip.
* **A signalk-server** (2.x — espOS is verified against 2.31) on the same
  network, with security enabled, which is the default. Without security
  the device streams without a token and step 4 does not apply.
* `git`. Node is **not** needed — the web UI bundle is committed — and
  Python is IDF's own. `libbsd-dev` is for the host tests only
  ([Development](development.md)), not for a device.

## 1. Clone and build the minimal example

```sh
git clone https://github.com/signalk-espOS/espOS.git
cd espOS/components/espos_core/examples/minimal
idf.py set-target esp32c6
idf.py build flash monitor
```

Every example is a complete ESP-IDF project, so `idf.py` works inside its
directory as in any other. Two lines of the first configure are expected: a
warning that a *development* signing key was generated (every image is
signed; [OTA](ota.md#signing-key)), and `partition table
…/partitions/4mb.csv`. If `flash` cannot find the port, add
`-p /dev/ttyACM0` (or `/dev/ttyUSB0`). On a shared or small machine use
`/path/to/espOS/scripts/build.sh build` in place of `idf.py build` — one lock
per machine, half the cores ([Development](development.md)).

## 2. What the monitor prints, in order

The device narrates itself. After IDF's own boot lines:

```text
I (612) espos: espOS 0.7.0 on esp32c6 — app minimal 0.7.0-12-gabc1234
I (702) espos_wifi: no network configured: join "espOS-1a2b" and open http://192.168.4.1
I (710) minimal: publishing environment.inside.temperature every second
```

The banner names espOS's version and the target, then the application and
*its* version — `git describe`, so a build between two releases is
distinguishable from the release. Nothing is configured yet, so the
provisioning portal is up at once and the second line says what to do. The
third is the application: `espos_start()` has returned and it publishes
already — the values are batched and buffered until the stream is up, then
drained oldest first.

## 3. Join the portal and pick your WiFi

On a phone or laptop, join the open WiFi network **`espOS-1a2b`** (the four
hex digits are the end of the device's MAC and differ per device). Most
phones pop their "sign in to network" sheet by themselves; otherwise open
`http://192.168.4.1`. The page scans, you pick your network and enter its
password, save. The portal drops the moment the station connects — your
phone loses the `espOS-…` network, which is expected — and the monitor goes
on:

```text
I (9310) espos_wifi: connected to "Boat" as 192.168.1.23 — web UI: http://espos-1a2b.local
I (9420) espos_sk: found signalk-server "boat" at 192.168.1.10:3000 (discovered urn:mrn:signalk:uuid:…)
I (9650) espos_sk: access requested — approve it in the server UI: Security → Access Requests
```

The device found the server by mDNS (`_signalk-http._tcp`) and posted an
access request. From now on its own web UI answers at
`http://espos-1a2b.local`: status, WiFi, Signal K, settings, logs, OTA.

## 4. Approve the device in Signal K

Open the server's admin UI and go to its **Security → Access Requests**
page. The request is listed by the device's description — the application
name and hostname, `minimal espos-1a2b` — and its client id. Give it
**read/write** (publishing deltas needs it) and approve. The device polls
for the decision and says:

```text
I (17040) espos_sk: approved, streaming
```

The token is stored in NVS, keyed by the server's identity, and survives
reboots and re-flashing the application; a factory reset or `POST
/api/v1/sk/forget` discards it. Denied by mistake? Nothing retries by
itself; **Request again** on the device's Signal K page does
([Troubleshooting](troubleshooting.md#signal-k)).

## 5. Find the value

In the server's **Data Browser**, look under `vessels.self` for
`environment.inside.temperature`: `293.65` — Signal K stores SI units, so
kelvin, 20.5 °C — refreshed every second. The source column reads
`espos.<hostname>`, so with several devices you know which one a value came
from. The example's `main.c` is 40 lines; the constant is where a sensor
read goes. Next to it every espOS device publishes its own health every 10 s
under `espos.<hostname>.*` — uptime, heap, RSSI, reconnect counts — so a
dashboard sees the device without any application code
([Signal K → Delta stream](signalk.md#delta-stream-m4)).

## Next steps

* **A setting of your own.** One entry in a JSON descriptor becomes a key
  constant, a validated value and a field on the settings page:
  [Add a setting](tutorials/add-a-setting.md); example `custom_settings`.
* **Your own sensor.** Read it on your task, publish with
  `espos_sk_publish_number()`, declare its metadata:
  [Your first sensor](tutorials/first-sensor.md); example `analog_input`.
* **The other examples**, one concept each, all built the same way:
  [Examples](examples.md).
* **How it fits together** — the start order, which task calls you back and
  what you may do there: [Concepts](concepts.md). Read it before writing the
  first callback.
* **Coming from SensESP:** [Migrating from SensESP](migration-from-sensesp.md).
