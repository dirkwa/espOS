# Development

## Prerequisites

* ESP-IDF exactly `.idf-version` (v6.0.2) installed and exported
  (`. $IDF_PATH/export.sh            # ESP-IDF v6.0.2, see .idf-version`). The top-level CMake refuses a different
  version unless `-DESPOS_ALLOW_IDF_MISMATCH=1`.
* For host tests: `libbsd-dev` (Debian/Ubuntu) — required by IDF's linux
  target.
* Python ≥ 3.10 (IDF's own venv is used at build time; the generator needs
  only the standard library).

## Build & flash the example app

```sh
idf.py set-target esp32c6           # any of: esp32 esp32s3 esp32c3 esp32c6 esp32p4
idf.py menuconfig                   # (M1 only) espOS example app → WiFi SSID/password
idf.py build flash monitor
```

Multi-target check without touching the main build dir:

```sh
for t in esp32 esp32s3 esp32c3 esp32c6 esp32p4; do
  idf.py -B build-$t -DSDKCONFIG=build-$t/sdkconfig -DIDF_TARGET=$t build || break
done
```

## Host tests (no hardware)

```sh
cd test/host/espos_config_test
idf.py --preview set-target linux && idf.py build
./build/espos_config_test.elf              # Unity; exit code 0 == pass

cd ../espos_httpd_test
idf.py --preview set-target linux && idf.py build
python3 run_test.py                        # drives the real REST server over HTTP
```

`tools/espos_gen_config.py` has its own tests: `python3 -m unittest
discover -s tools -p 'test_*.py'`.

## Repository layout

```
components/espos_config/   NVS-backed config store, descriptor tables, JSON, migrations
components/espos_httpd/    esp_http_server + REST API + static UI
main/                      example app (+ M1-only WiFi bootstrap)
tools/                     build-time generators
test/host/                 linux-target unit/integration tests
docs/                      contracts and guides
ui/                        Vite SPA (M5)
```

## Conventions

* Small, reviewable commits; one milestone per branch.
* Ask before adding a third-party dependency (`idf_component.yml`).
* `docs/api.md` is a contract; changes there are discussed first.
* No absolute paths or machine-specific config in committed files.
* Warnings are errors (IDF 6 default); keep the build clean.
