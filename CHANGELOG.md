# Changelog

All notable changes to espOS. The format is
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions are
semantic-ish as judged from a consumer firmware's point of view
([docs/releasing.md](docs/releasing.md)) — espOS is pre-1.0, so a *minor*
bump is where a consumer may have to change code, and such entries say so.

Entries name the component the way commit scopes do (`wifi`, `sk`, `ble`,
…); the PR number is the place to read the reasoning.

## [Unreleased]

### Changed

- build: consumers inherit espOS's sdkconfig instead of copying it.
  `espos_project_prologue()` assembles `SDKCONFIG_DEFAULTS` from
  `sdkconfig.d/espos.defaults` (+ `.<target>`), an optional profile
  (`PROFILE release|debug` or `-DESPOS_PROFILE=`), the project's
  `sdkconfig.defaults`, a git-ignored `sdkconfig.local`, and a generated
  partition fragment. **Consumers:** delete the mirrored espOS lines from your
  `sdkconfig.defaults` and pass `PARTITIONS <csv>` (default
  `partitions/4mb.csv`; `8mb.csv` and `16mb.csv` are bundled and set the flash
  size) — the prologue's table wins over a `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME`
  in your defaults. Delete an existing `build/sdkconfig` once after the bump:
  defaults only apply to a fresh sdkconfig, and a stale one still names the
  removed root `partitions.csv`.
- build: consumer projects build with IDF's `MINIMAL_BUILD`; espOS's own tree
  still compiles every component. **Consumers:** name the optional espOS
  components you use — `espos_project_prologue(... COMPONENTS espos_ble)` —
  the rest (`espos_ble`, `espos_n2k`, `espos_voice`, `espos_audio`) are excluded
  outright, because the component manager resolves every visible manifest
  before the build graph is trimmed and `espos_voice` alone drags esp-sr,
  esp-dl and esp-dsp into a headless gateway's lock (0 members linked). `sdkconfig.defaults*` moved to
  `sdkconfig.d/espos.defaults*`, `partitions.csv` to `partitions/4mb.csv`.
- build: IDF version policy — `.idf-version` is the tested release, any 6.0.x
  builds with one warning, versions outside `[6.0.0, 6.1.0)` are refused
  unless `-DESPOS_ALLOW_IDF_MISMATCH=1`.
- ui: `ui/dist-gz` is committed; firmware builds no longer need Node, a
  missing default bundle is a hard error, CI checks the bundle is fresh.
- httpd, wifi, sk, ota: `*_start()` fails with `ESP_ERR_INVALID_STATE` and a
  log line naming the missing prerequisite when called out of order.
- Boot log narrates version and target, portal instructions, connection and
  web UI URL, server found, access request, approval.
- The reference app in `main/` is built on `espos_start()`; config is read
  once and on change.
- sk: the `lowMemory` check moved out of the SignalK health tick into
  `espos_health`, so it exists without SignalK; the token legs and meta
  reconciliation share the new HTTP client. `CONFIG_ESPOS_HEALTH_MAX_CONDITIONS`
  default 8 → 12.
- sk: discovery no longer starts the responder or sets the hostname (both
  moved to `espos_wifi`); a browse waits for `espos_mdns_is_ready()`
  (bounded), returns nothing without a link, and is compiled out with
  `CONFIG_ESPOS_WIFI_MDNS=n` (set `sk.server_host`). The `espressif/mdns`
  dependency moved from `espos_sk` to `espos_wifi` as a public `REQUIRES`.
- ble: advertisements are posted through `espos_sk_http_post()` and the
  control WebSocket sends the token as an `Authorization` header instead of
  `?token=`; both channels follow `sk.tls` (https/wss) and the socket
  re-dials on a scheme change.
- Relicensed from the source-available, no-redistribution license to
  Apache-2.0: `LICENSE`, `NOTICE`, SPDX headers on every source file,
  `REUSE.toml` for the rest, `THIRD-PARTY-NOTICES.md` for what espOS links
  against. Redistribution of espOS and of firmware built on it is now
  permitted under the Apache-2.0 terms.
- Shared build wrapper `scripts/build.sh`: one lock per machine, half the
  cores, `nice`/`ionice`; consumers call it through the submodule. The
  documented build commands now go through it.
- config: the descriptor generator moved from `tools/` into
  `components/espos_config/tools/` so a registry-installed copy of the
  component is self-contained; `tools/espos_gen_config.py` is a shim for one
  release. The UI mock and the docs point at the new path.
- docs: `docs/api.md` is now `docs/rest-api.md` (it is the REST contract, not
  the C API); `docs/signalk.md` no longer names a Kconfig symbol that never
  existed and states the real server-list size; `docs/development.md` describes
  `test/host/run_all.sh` discovery instead of a stale project list; the README
  milestone table became a short status and the plan-vs-implementation notes
  moved to `docs/decisions.md`.

### Added

- `espos_core`: `espos_start()` one-call boot (log → config → httpd → wifi →
  sk → ota → ble, optional stacks only when built), `espos_init()`,
  `espos_start_network()`, `espos_version()`, `espos_app_name()`; Kconfig
  `ESPOS_CORE_HEALTH_WATCHDOG` (reserved for the health policy).
- `espos_event`: the `ESPOS_EVENT` base on the default loop with
  `CONFIG_READY`, `HTTPD_STARTED`, `NETWORK_UP/DOWN`, `SK_SERVER_SELECTED`,
  `SK_TOKEN_APPROVED`, `OTA_AVAILABLE` (`MDNS_READY`, `SK_STREAM_*` declared);
  `espos_event_post/subscribe/unsubscribe`.
- `espos_config_is_ready()`.
- wifi: mDNS responder with an API (`espos_mdns.h`). The device answers for
  `<wifi.hostname>.local` and advertises `_http._tcp` (TXT `path=/`) and
  `_espos._tcp` (TXT `v`, `app`, `espos`, `target`, `id`, `api=/api/v1`,
  `auth=0`) on `httpd.port`. `espos_mdns_add_service()` /
  `espos_mdns_remove_service()` register application services at any time
  (queued until the responder is up, `CONFIG_ESPOS_WIFI_MDNS_MAX_SERVICES`
  slots, default 6); `espos_mdns_is_ready()` and `ESPOS_EVENT_MDNS_READY`
  (posted on every `NETWORK_UP`) say when the network can be reached.
  `CONFIG_ESPOS_WIFI_MDNS` (default y) builds it; off, or on the linux target,
  the API compiles to stubs. Replaces consumers' retry loops around
  `mdns_service_add()`.
- core: `ESPOS_ABI_VERSION` (1) in `espos.h` and `espos_abi_version()`. The C
  headers under `components/*/include` are the stable contract a binding is
  generated from; rules in docs/development.md "Public API rules", the
  decision (C ABI, C++ facade above it, Rust not now) in docs/decisions.md.
- tools: `tools/check_public_headers.py` checks the public headers against
  those rules — IDF includes beyond `esp_err.h` and the two frozen exceptions,
  `#pragma once`/`extern "C"`, `CONFIG_` tokens as warnings; CI runs it as the
  `headers` job.
- health: the device watchdog policy. `espos_health_policy_start()` (armed by
  `espos_start()`) ticks every 10 s, raises `lowMemory` (WARN 40 KB total /
  20 KB internal; fatal ALARM 12 KB internal / 8 KB largest block) and
  `taskStalled`, and restarts after 3 consecutive ticks with a condition
  raised via `espos_health_report_ex(..., ESPOS_HEALTH_F_REBOOT_ON_ALARM)` in
  ALARM. WARN and unflagged ALARM never restart. Pure C behind a port
  (`espos_health_policy.h`), host-tested.
- health: `espos_health_watch_task()/kick()/unwatch_task()` subscribe a task to
  the IDF task watchdog (now 30 s, panic → core dump → reboot, rollback
  before OTA confirm) and to the policy; the SignalK stream task is watched.
- health: reset record — `espos_health_last_reset()`; `GET /api/v1/system/info`
  gains `last_reset` (`null` unless the watchdog restarted the device).
- core: `netDown` WARN/NORMAL from `ESPOS_EVENT_NETWORK_DOWN/UP`, never fatal
  by construction.
- sk: `skLinkStalled` fatal condition (WiFi up, stream worked once, down for
  `sk.stall_s` — new key, default 300 s); `ESPOS_EVENT_SK_STREAM_CONNECTED/
  DISCONNECTED` are posted; telemetry adds `espos.<label>.{internalFree,
  largestBlock}`.
- sk: `espos_sk_http.h`, one HTTP client for the selected server:
  `espos_sk_http_get/put/post/delete`, `espos_sk_get_value/meta`,
  `espos_sk_url/ws_url`; fresh `esp_http_client` per call and `perform()` only
  (avoids the two `http_on_body` asserts seen on the P4), body cap with
  `truncated`, Bearer from the token snapshot, scheme from `sk.tls`, 401/403
  reported to the token machine. `CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT`
  (default 2) bounds requests in flight.
- build: `sdkconfig.d/release.defaults` (flash + NVS encryption) and
  `debug.defaults`; `partitions/8mb.csv`, `partitions/16mb.csv`;
  `components/espos_core/project_include.cmake` lints a registry consumer's
  sdkconfig (event/timer task stacks; P4 L2 cache line vs hosted mempool, BA
  window vs PSRAM); `scripts/build.sh` enables ccache when installed.
- Contributor tooling: `.clang-format` derived from the existing C style and
  a Google-style one for the C++ components, an advisory `.clang-tidy`,
  `scripts/check_no_secrets.sh`, `tools/espos_size_diff.py` (size report and
  app-slot budget from `idf.py size --format json2`), Dependabot, issue and
  pull-request templates, `CODEOWNERS`, `SECURITY.md`, `CONTRIBUTING.md`,
  release-notes categories, and this changelog.
- ota, ble, n2k, voice: `Kconfig` menus for the knobs that were literals
  (manifest size, boot-check delay, task stacks, advert batch, GATT sessions,
  candump port, RX queue depth, the legacy `_sensesp-n2k._tcp` service type,
  Wyoming port); defaults equal the former values.
- `tools/check_kconfig_docs.py` and a CI job: every `CONFIG_ESPOS_*` the docs
  mention must exist in a `Kconfig`.
- CI: `reuse lint`, clang-format on changed files, a tracked-secrets check,
  a per-target size report with a 90 % app-slot budget, and a component
  manifest pack dry-run.
- Espressif Component Registry manifests for all eleven components (namespace
  `espos`, version in lockstep with `version.txt`, sibling dependencies via
  `override_path`, `idf >=6.0.0,<6.1.0`); cJSON declared in `espos_sk`,
  `espos_wifi`, `espos_ota` and `esp_hosted` (P4) in `espos_wifi`, which their
  CMakeLists already required. `docs/releasing.md` gains "Registry publishing".

### Fixed

- config: `espos_gen_config.py` emits valid empty tables and an empty schema
  when no descriptor is registered (was a build error for a consumer that
  declares none).
- ble: report allocatable heap, and never wait on the BT task (#35).

## [0.7.0] - 2026-09-06

### Fixed

- voice: gate on-device wake during playback and the echo tail, so the
  satellite does not wake on its own reply (#34).
- wifi: the esp_hosted transport mempool prefers PSRAM on the ESP32-P4,
  keeping internal RAM for what needs it (#33).
- voice: hold the wake stream on the device's own voice; skip only the
  on-device engine, not the network one (#32).
- release: a first tag gets release notes, and the release is published on
  GitHub (#31).

## [0.6.0] - 2026-08-28

First tagged release: config store, HTTP server and REST API, WiFi state
machine with captive portal, SignalK discovery, token and delta stream in
both directions, web UI, device health, signed OTA with rollback, and the
BLE, NMEA 2000 and voice components. Earlier history is in git.

[Unreleased]: https://github.com/signalk-espOS/espOS/compare/v0.7.0...HEAD
[0.7.0]: https://github.com/signalk-espOS/espOS/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/signalk-espOS/espOS/releases/tag/v0.6.0
