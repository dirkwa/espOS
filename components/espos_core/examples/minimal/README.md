# minimal — **Essential**

The whole of an espOS application: `espos_start(NULL)`, then publish. One
Signal K value, `environment.inside.temperature`, once a second; a constant
(293.65 K = 20.5 °C) stands in for the sensor read. Replaces SensESP's
`minimal_app` and `constant_sensor`. No wiring; any supported board (ESP32,
ESP32-S3, ESP32-C3, ESP32-C6, ESP32-P4).

## The ten-minute path

```sh
. $IDF_PATH/export.sh                 # ESP-IDF v6.0.2, see .idf-version at the repo root
cd components/espos_core/examples/minimal
idf.py set-target esp32c6             # or esp32 / esp32s3 / esp32c3 / esp32p4
idf.py build flash monitor            # small or shared host: ../../../../scripts/build.sh build, then idf.py flash monitor
```

The first configure warns that it generated a development app-signing key
(`secure_boot_signing_key.pem`, git-ignored): right for a bench, docs/ota.md
before a device leaves it. What the monitor then shows, in order:

1. `espOS 0.7.0 on esp32c6 — app minimal <version>`, the boot banner, and
   `watchdog armed: tick 10 s, 3 strikes; ...` from the health policy.
2. `no network configured: join "espOS-1a2b" and open http://192.168.4.1` —
   join that access point from a phone; the portal page (most phones open it
   as a "sign in to network" sheet) scans, you pick your WiFi and type its
   password. `1a2b` is the device's id, the last two bytes of its MAC.
3. `connected to "<ssid>" as 192.168.1.23 — web UI: http://espos-1a2b.local` —
   the portal drops and the phone loses the AP; expected. The web UI (status,
   WiFi, SignalK, config, logs, OTA) is at that address from now on.
4. `found signalk-server "<name>" at 192.168.1.10:3000 (...)` — mDNS found it.
5. `access requested — approve it in the server UI: Security → Access Requests`
   — in signalk-server's admin UI open **Security → Access Requests** and
   approve `minimal espos-1a2b` with read/write permission.
6. `approved, streaming` — from here on every second's value goes out.
7. `minimal: publishing environment.inside.temperature every second` was
   logged right after `espos_start()` returned; the values published while
   steps 2–6 were still happening were buffered, not lost, and drain now.

Where the value appears: signalk-server → **Data Browser**, filter on
`environment.inside.temperature`; the source is `espos.espos-1a2b`. It reads
293.65 because Signal K is SI (kelvin); the server converts for display.

## What is in it

* `CMakeLists.txt` — includes the shared prologue (`cmake/espos_project.cmake`), which does everything a project needs before `project()`.
* `main/CMakeLists.txt` — `PRIV_REQUIRES espos_core espos_sk espos_ota`: naming a component is what builds it, and `espos_start()` starts what is built.
* `main/main.c` — 39 lines, half of them comments.
* `main/idf_component.yml` — the IDF pin and the two ESP32-P4 co-processor entries; nothing else.

## As a template

This directory is what a new firmware starts from: copy it, add espOS as a
submodule at `espos/`, change the include in `CMakeLists.txt` to
`espos/cmake/espos_project.cmake`, rename the project (docs/development.md,
"Building a firmware on espOS").
