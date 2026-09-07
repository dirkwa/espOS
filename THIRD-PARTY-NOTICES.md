# Third-party notices

espOS itself is Apache-2.0 (see [LICENSE](LICENSE) and [NOTICE](NOTICE)). The
build pulls in the components below, each under its own license. Versions are
the ones pinned in `dependencies.lock` / `ui/package-lock.json` at the time of
writing; the lock files are authoritative.

| Component | Used by | License |
|---|---|---|
| [ESP-IDF](https://github.com/espressif/esp-idf) 6.0.x | everything | Apache-2.0 |
| [espressif/cjson](https://components.espressif.com/components/espressif/cjson) (cJSON) | config, httpd, sk, ota, ble, voice | MIT |
| [espressif/mdns](https://components.espressif.com/components/espressif/mdns) | wifi (responder, `espos_mdns.h`), sk (discovery), n2k | Apache-2.0 |
| [joltwallet/littlefs](https://components.espressif.com/components/joltwallet/littlefs) | httpd (UI partition) | BSD-3-Clause (littlefs) / Apache-2.0 (port) |
| [espressif/esp_websocket_client](https://components.espressif.com/components/espressif/esp_websocket_client) | ble | Apache-2.0 |
| [espressif/esp_hosted](https://components.espressif.com/components/espressif/esp_hosted), [espressif/esp_wifi_remote](https://components.espressif.com/components/espressif/esp_wifi_remote) | wifi/ble on ESP32-P4 | Apache-2.0 |
| [espressif/esp-sr](https://components.espressif.com/components/espressif/esp-sr) (WakeNet/AFE) | voice (esp32s3, esp32p4) | Apache-2.0 for the API; the wake-word **model binaries are under Espressif's own model license** — read it before redistributing a voice firmware image |
| [espressif/esp-dl](https://components.espressif.com/components/espressif/esp-dl), [espressif/esp-dsp](https://components.espressif.com/components/espressif/esp-dsp), [espressif/dl_fft](https://components.espressif.com/components/espressif/dl_fft) | pulled in by esp-sr | MIT (esp-dl) / Apache-2.0 (esp-dsp, dl_fft) |
| [espressif/esp_new_jpeg](https://components.espressif.com/components/espressif/esp_new_jpeg) | pulled in by esp-sr | Espressif proprietary (binary) — check the component's LICENSE |
| [Unity](https://github.com/ThrowTheSwitch/Unity) (via ESP-IDF) | host tests | MIT |
| [preact](https://github.com/preactjs/preact) | web UI (runtime) | MIT |
| [vite](https://github.com/vitejs/vite), [@preact/preset-vite](https://github.com/preactjs/preset-vite), [typescript](https://github.com/microsoft/TypeScript) | web UI (build time only) | MIT / MIT / Apache-2.0 |

The wireless drivers linked by ESP-IDF (`esp_wifi`, `bt`, `esp_phy`) are
Espressif binary libraries under Espressif's MIT-style license shipped with
ESP-IDF.
