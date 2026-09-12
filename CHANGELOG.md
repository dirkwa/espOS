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

- partitions: the `nvs` partition is 48K, was 24K, in all three shipped tables.
  A graph node keeps its settings there and a curve table is one string key of
  up to about 4 KB, so the release that introduced editable tables is the one
  that has to size the partition for them: the size is fixed when the table is
  flashed and growing it later needs a USB flash, not an OTA. **Existing
  devices are not affected by an update** — an OTA writes the app slot, not the
  partition table, so a device in the field keeps the layout it was flashed
  with, and espOS resolves every partition by label rather than offset. A
  device only takes the new table when it is flashed over USB, which erases
  NVS anyway. **Consumers with their own partition CSV** should make the same
  change; in both espOS consumers the three small data partitions after `nvs`
  shifted into space that was already spare before `ota_0`, so no application
  or data partition moved.

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

- prov: `espos_prov`, BLE provisioning -- WiFi credentials from a phone over
  GATT, with no access point and no captive portal. Off by default
  (`CONFIG_ESPOS_PROV`, about 25 KB of flash).

  **Credentials only: espOS keeps the radio.** Credentials arriving over BLE
  are written to the `wifi` config namespace -- the same keys the web UI
  writes -- and the existing WiFi state machine connects as it always does.
  Nothing in this component calls `esp_wifi_*`. That is why it is built on
  protocomm directly rather than on `espressif/network_provisioning`: that
  component's manager drives the station itself (`esp_wifi_set_storage`,
  `esp_wifi_set_config` and a connect timer, all *before* it invokes the
  application's callback, with no Kconfig to turn it off), which would mean
  two owners of one radio and two stores of one set of credentials.

  The cost of that choice is compatibility: Espressif's *ESP BLE
  Provisioning* phone app speaks the manager's protobuf schema and will not
  talk to this. The endpoint here is plain JSON.

  **Known limitation, unresolved:** on a firmware that also runs
  `espos_ble`, `protocomm_ble_start()` fails `ESP_ERR_INVALID_STATE` with
  `Bluedroid already initialised`. protocomm's `simple_ble_start()` calls
  `esp_bluedroid_init_with_cfg()`/`esp_bluedroid_enable()` unconditionally --
  it assumes it owns the whole stack -- and the gateway has already brought
  it up. Provisioning and the BLE *gateway* therefore cannot both run in one
  firmware yet; the failure is non-fatal and the device carries on scanning.
  A provisioning-only firmware is unaffected.

- `espos_flow`: the data-flow runtime — one loop task, a wrap-safe timer wheel,
  a cross-task mailbox, and a typed producer/consumer graph over them. Nodes are
  wired with `connect_to()` or `>>`, cost no allocation after start-up and need
  no locks, because everything in a graph runs on one task and everything else
  enters through a `Mailbox`. `Poll`, `Lambda`, `Join`, `Sink`, `Value`,
  `Constant`, `Ticker` and `Mailbox`; `Graph::make<T>()` or value semantics for
  ownership. A firmware that does not require the component links zero bytes of
  it (measured: the minimal example is byte-identical). See docs/flow.md.
- `espos_formulas`: the marine arithmetic as a header-only component with no
  dependencies at all — SI conversions for every unit the Signal K spec uses,
  curve interpolation, dew point (Arden Buck), heat index (the full NOAA
  regression with both adjustments), air density, resistive dividers, tank
  level and battery state of charge. It is separate from `espos_flow` on
  purpose: the formulas are testable, and tested, without a graph, a task or a
  chip. See docs/transforms.md.
- sk: **inbound PUT** — a Signal K client can operate a switch on an espOS
  device, which was simply not possible before: the stream parser never looked
  at `put`. `espos_sk_put_handler_register()` / `_unregister()` answer a request
  automatically (200, 400 for an unusable value, 502, and 405 for a path with no
  handler), or return `ESPOS_SK_PUT_PENDING` and answer later with
  `espos_sk_put_respond()`. Both wire forms are accepted: the **array** form
  signalk-server actually sends and the single-object form a client sends.
  Every request is answered, because silence costs the client a 60-second
  timeout. Note the server routes a PUT on `(path, $source)` it has *observed*,
  so a device must publish a path before it can be written to.
- sk: `espos_sk_flush(timeout_ms)` drains buffered deltas — the prerequisite for
  sleeping without losing the last samples. `ESPOS_SK_MAX_META` 16 → 32, now
  `CONFIG_ESPOS_SK_MAX_META`. `/api/v1/sk/status` gains `puts_in` and
  `puts_rejected`.
- `espos_sensors`: the drivers a sensor firmware starts from, on IDF 6 APIs and
  as flow nodes — calibrated ADC reads in volts, GPIO in and out, pulse counting
  over PCNT **with a GPIO-interrupt fallback on SoCs that have no PCNT unit**
  (the ESP32-C3), LEDC PWM, a shared I2C bus for breakouts, and optional
  1-Wire/DS18B20 (`CONFIG_ESPOS_SENSORS_ONEWIRE`, off by default and costing
  nothing when off). See docs/sensors.md.
- `espos_sk_flow`: the Signal K nodes for the graph — `Output`, `Listener`,
  `PutHandler`, `PutRequest`, `Notify`, `NetRssi`, `IpAddress`. `Output` takes
  metadata only together with a custom path, so the rule that spec paths never
  carry meta cannot be broken by accident. The `smart_switch` example is the one
  that could not be written before inbound PUT existed.
- `espos_flow` transforms: 33 nodes covering what a SensESP firmware wires
  daily — `Linear`, `MovingAverage`, `Median`, `Ema`, `ChangeFilter`,
  `Debounce`, `Throttle`, `Threshold`, `Hysteresis`, `Curve`, `Frequency`,
  `RunHours`, `Expire`, `Repeat`, `Clamp`, `Deadband`, `RateOfChange`,
  `MinMaxHold`, `Latch`, `Counter`, `WrapAngle`, `Convert` and the rest. A
  node's constant can be bound to a config namespace with `Param`, which makes
  it editable in the web UI, and the time-stamping transforms use `espos_time`
  when the firmware builds it; both are optional dependencies and the headers
  compile without them.
- Config namespaces registered at run time: `espos_config_register_ns()` /
  `espos_config_unregister_ns()` let a graph node publish its own settings
  (multiplier, offset, curve table) to the web UI and NVS without a
  build-time descriptor. A node id of at most 12 characters becomes the NVS
  namespace `f_<id>`; a duplicate, over-long or malformed one fails loudly
  and raises the health condition `flowConfig`.
- `espos_config_schema_json()` serves the compiled schema merged with the
  runtime namespaces, under an ETag that moves on every register and
  unregister; `GET /api/v1/config/schema` and `/system/info`'s `schema_etag`
  both use it.
- Descriptor fields `readOnly`, `group` and the presentation block `x`
  (`displayMultiplier`, `displayOffset`, `format: "table"` with `columns`),
  accepted for build-time descriptors as well. Tables are string keys holding
  JSON, so an export stays readable and the UI edits rows without base64.
- Web UI: groups render as tabs, read-only fields are shown but not editable,
  display multiplier and offset are applied on read and inverted on write,
  and table keys get a row editor.
- `CONFIG_ESPOS_CONFIG_MAX_RUNTIME_NS` (default 32).

- time: new `espos_time` component — the device's wall clock, learned from
  SNTP, a manual `PUT /api/v1/time`, `navigation.datetime` on the SignalK
  stream, or an instant carried through a deep sleep in RTC memory, ranked in
  that order so a coarse source never walks back one that is better. SNTP is
  armed at start and begins polling on the first `NETWORK_UP`, whichever
  transport produces it. `espos_time_now_ms()` returns 0 while unsynced rather
  than a plausible-looking 1970, so a missing time reads as missing.
  `GET`/`PUT /api/v1/time`, a `time` object in `/system/info`, a Clock line on
  the status page, config `time.*`, and `ESPOS_EVENT_TIME_SYNCED`
  ([docs/time.md](docs/time.md)).
- sk: deltas carry the time their values were **measured**
  (`updates[].timestamp`, config `sk.timestamps`, default on). The engine
  records a monotonic stamp per queued message and converts it at send time, so
  a message buffered through an outage keeps its own time even when the clock
  was only set afterwards — previously the server stamped it on arrival and an
  hour of data landed as one burst at reconnect.
- log: `CONFIG_ESPOS_LOG_WALLCLOCK` (default on) adds the UTC time to stored
  log lines beside the existing milliseconds-since-boot. Console output and
  lines logged before the clock is set are unchanged.
- sk: TLS to real boat servers. A trust store next to the token (NVS
  `skstate`) pins what the server presents on the first connection that works
  and holds it to that afterwards, the way ssh does — so signalk-server's own
  self-signed certificate, and any private CA, now work instead of being
  refused. A CA anchor binds the issuing CA *and* the leaf's SAN set, so a
  renewal by the same CA for the same names is accepted with nobody pressing
  anything; a bare self-signed certificate is pinned as itself and a
  replacement needs one deliberate "trust the new certificate". There is no
  accept-anything mode. `GET`/`DELETE /api/v1/sk/tls`,
  `PUT /api/v1/sk/tls/ca`, SSE `sk_tls`.
- sk: token state `cert_error` — the certificate is not the trusted one. The
  token is kept (it is the transport that is wrong, not the credential), the
  retry is a flat 60 s rather than an exponential backoff, and the stream stays
  down rather than falling back to plaintext. Health condition `skCertificate`.
  One TLS handshake at a time device-wide, with a pre-flight check on
  contiguous internal RAM (`tlsMemory` WARN when it defers).
- `espos_net`: the interface-agnostic network seam — default route (Ethernet
  before WiFi before Thread), `espos_net_get_status/is_up/subscribe/short_id/
  backoff_ms`, transports plug in through `espos_net_register_if/report`;
  `GET /api/v1/net/status` and the `net` SSE event; `net.hostname`; the mDNS
  responder moved here unchanged. `espos_sk` and `espos_ota` depend on
  `espos_net`, not on `espos_wifi`; a headless esp32h2 build (no WiFi at all)
  is a CI gate. `docs/net.md`.
- wifi: static IP (`wifi.ip_mode`, `ip`, `netmask`, `gateway`, `dns0`,
  `dns1`), applied before every connect.
- httpd: REST authentication (closes #2). `httpd.api_key` (secret; empty =
  open, the default) guards every endpoint registered through
  `espos_httpd_register()`: `Authorization: Bearer <key>` for machine clients,
  or the `espos_sid` cookie from `POST /api/v1/auth/login` (`/logout`,
  `/status`) for browsers; cookie writes need a matching `Origin`; five wrong
  keys in 60 s → 429 for 30 s; the setup-portal network is exempt (lockout
  recovery). New `espos_httpd_register_ex(uri, ESPOS_HTTPD_PUBLIC|PROTECTED)`,
  `espos_httpd_request_authenticated()`, `espos_httpd_auth_policy.h`; public
  `GET /api/v1/system/ping`; `GET /system/info` is now protected. Kconfig
  `ESPOS_HTTPD_AUTH_REQUIRED`, `ESPOS_HTTPD_MAX_SESSIONS`, `ESPOS_HTTPD_TLS`
  (reserved). **Consumers:** an application's endpoints become protected
  automatically; an endpoint that must stay open registers with
  `ESPOS_HTTPD_PUBLIC`; the cockpit's own :8081 server is unaffected.
- ui: login page, Log out, a Generate button for `httpd.api_key` (shown once),
  an Access card on Status; the mock serves `/auth/*` and `/system/ping`.

### Changed

- sk: **`sk.tls` (bool) is replaced by `sk.scheme` (auto | http | https,
  default `auto`)**; the descriptor is at version 2 and a migration maps
  `tls = true` to `"https"` and anything else to `"auto"`. **Consumers:** a
  config export or an automation that writes `sk.tls` must write `sk.scheme`
  instead. `auto` reads the scheme off the server's own mDNS advertisement, or
  for a manual host off one unauthenticated redirect probe. The setting is no
  longer `restart_required`.
- sk: **`CONFIG_ESPOS_SK_TLS` now defaults to `y`.** Measured on esp32c6:
  about 10 KB with `espos_ota` in the build (mbedTLS and the bundle are
  already there for the https image source), about 78 KB without it. New
  setting `sk.tls_trust` (tofu | ca | bundle, **default `tofu`**); `bundle` is
  the previous behaviour, and `sk.ca_pem` supplies the CA for `ca` mode so a
  fleet can connect verified on the first try.
- sk: over plaintext a single 401 no longer clears the access token. Anything
  on the path can answer one — a captive portal, a proxy, a router's login
  page — and replacing a token costs a trip to the server's admin UI, so the
  first refusal buys a re-check 5 s later and only two in a row clear it. Over
  TLS one is still conclusive. On a TLS server the token machine also skips
  the VERIFYING leg, which only bought a second handshake per reconnect.
- **Consumers, reflash required:** `partitions/4mb.csv` moves 256 KB from
  `storage` into the two app slots (`ota_0`/`ota_1` 1600K → 1728K, `storage`
  640K → 384K). A full build — WiFi, TLS-capable crypto, mDNS, LittleFS and
  the web UI — had reached 92 % of the old slot, leaving no room for the next
  feature or for an OTA image briefly larger than the running one; the UI
  bundle is 22 KB, so 384 KB is still seventeen times what it needs. A device
  on the old table keeps working, but takes the new layout only through a
  USB flash, not over the air.
- **Consumers:** `espos_wifi_start()` requires `espos_net_start()` first
  (`espos_start()` handles it: httpd → net → wifi → …). `espos_wifi_short_id()`
  and `espos_wifi_backoff_ms()` are deprecated, removed in 0.9 — use
  `espos_net_*`. `wifi.hostname` → `net.hostname` (migrated once at boot).
  `CONFIG_ESPOS_WIFI_MDNS[_MAX_SERVICES]` → `CONFIG_ESPOS_NET_MDNS[_MAX_SERVICES]`.
  The device id is the base MAC on every transport; on the ESP32-P4 the
  default hostname, portal SSID and Signal K source label change once — set
  `net.hostname` to keep a name. Delete `build/sdkconfig` once after the bump.

- examples: eleven buildable example projects under `components/<c>/examples/`,
  indexed in `examples/README.md`, each a complete IDF project on the shared
  prologue (≤ 120 lines of C) naming the SensESP example it replaces. Core:
  `minimal` (the project a new firmware starts from), `two_phase_boot`,
  `custom_settings`, `app_endpoint_and_page`, `health_and_led`; Signal K with
  real peripherals: `analog_input`, `pulse_counter`, `digital_switch`,
  `listener_relay`, `json_and_meta`, `tls_server`. `main/` stays the
  all-components reference app.
- wifi: the manifest names `espressif/esp_wifi_remote` for the ESP32-P4 next
  to `esp_hosted` (esp_hosted's own manifest declares only `idf`), so a
  consumer no longer repeats both lines; component manifests exclude example
  build state from registry packs.
- docs: documentation site at signalk-espos.github.io/espOS — mkdocs-material
  (`mkdocs.yml`, `docs/requirements.txt`), strict build on every pull request
  and deploy from `main` (`.github/workflows/docs.yml`); new pages: home,
  getting started, examples index, hardware, troubleshooting, roadmap, C API
  overview; the C API reference is generated from `components/*/include` by
  Doxygen (`Doxyfile`) through mkdoxy; the changelog page is rendered from this
  file (`docs/_hooks/root_files.py`).
- docs: `docs/migration-from-sensesp.md` — the three things that change coming
  from SensESP, a symbol-by-symbol table (SensESP → espOS today → planned
  facade), a worked port of SensESP's `analog_input.cpp`, what has no
  equivalent yet, what espOS adds. Six tutorials under `docs/tutorials/`
  (first-sensor, add-a-setting, tank-level, app-endpoint-and-ui-tab,
  ota-from-a-manifest, logs-and-core-dumps), labelled Essential / Newbie /
  Advanced. `concepts.md` gains "From SensESP's model to espOS's".
- Template: `scripts/sync_template.sh` generates the espos-template repository
  from the `minimal` example (espOS as the `espos/` submodule, a five-target
  CI, README with the ten-minute path); `scripts/build_example.sh` builds one
  example the way CI does. CI gains an `examples` matrix (every example on
  esp32c6, `minimal` on all five targets) and size caps for tutorials.
- build: `sdkconfig.d/debug.defaults` raises the maximum log level to debug so
  `PUT /api/v1/logs/level` can actually switch to it.
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

- ble/wifi: the setup portal was painfully slow to join on the ESP32-P4 --
  half a minute to get a DHCP lease, minutes to reach the page, sometimes
  never. The C6 co-processor is ONE radio serving both WiFi and BLE, and the
  gateway scans 160 ms out of every 320 ms, so the SoftAP lost half its
  beacons and the association and DHCP exchange with them. The scanner now
  stands down while the portal is up (new `ESPOS_EVENT_PORTAL_UP` /
  `PORTAL_DOWN`, and `espos_ble_scan_suspend()` / `_resume()`), and picks up
  again when the portal closes. A device showing its setup portal has nowhere
  to publish advertisements to yet, so nothing is lost.

  Two details that are easy to get wrong and are worth knowing if you add a
  second radio user. **Bluedroid keeps exactly one GAP callback** --
  `esp_ble_gap_register_callback()` is a setter, not a subscribe -- so a
  component that registers its own (protocomm's `simple_ble`, for instance)
  silently takes the gateway's, after which scan results stop arriving with
  no error reported anywhere; `espos_ble_scan_resume()` reclaims it. And
  **suspensions are counted**, because with a plain flag a second holder
  releasing its suspension handed the radio back while the portal still
  needed it -- observed on hardware as `suspended` followed by `resumed`
  150 ms later, with the portal still up.

  `GET /api/v1/ble/status` gains `scan_suspended`, so a device that has
  deliberately stopped scanning is distinguishable from one whose radio
  failed.

- httpd: the event-stream on-connect table held four callbacks while espOS
  itself ships five publishers (`net`, `wifi`, `sk`, `ota`, `ble`), so on a
  firmware with more than four the last to register never delivered the
  snapshot a fresh `GET /api/v1/events` client is documented to receive.
  Found on the BLE gateway, where `espos_ble` was the one that lost: it logged
  `status endpoint unavailable: ESP_ERR_NO_MEM` at boot and the web UI's BLE
  panel then stayed empty until the first periodic update. The symptom is
  quiet by construction — the REST endpoint still answers and later changes
  are still published, so the component reads as idle rather than
  unregistered, and the error names memory while nothing is short of memory.
  The limit is now `CONFIG_ESPOS_HTTPD_SSE_MAX_CONNECT_CBS` (default 8, was a
  hard-coded 4) and overflowing it is logged with the symbol to raise.
  **Consumers registering their own publishers** register after espOS's, so
  they are the ones that hit the cap; raise it rather than reordering.

- sk: a change of scheme was accepted by the configuration and then silently
  dropped by the token machine, whose "same server" test compared only host
  and port. A device switched between http and https kept using the old one.

- formulas: curve interpolation no longer extrapolates below its first sample.
  SensESP's `CurveInterpolator` divides by the gap between two points that do
  not exist for an input under the table's first x, publishing NaN to the
  server for what is usually a cold sender at rest (SensESP #1005); a
  duplicated x in the table is a second 0/0. Out-of-range inputs now clamp to
  the nearest sample and a duplicated x is rejected when the table is set.
- formulas: dew point uses the Arden Buck equation and is defined at 0 %
  humidity, where the Magnus form SensESP uses returns `-inf`; heat index
  applies both NOAA adjustments (low humidity at high temperature, high
  humidity in the mid 80s °F), which SensESP omits, and is only evaluated
  where the regression is valid.
- formulas: a resistive divider with an open circuit — a disconnected sender,
  the single most common failure on a boat — returns no value instead of
  infinity. SensESP publishes the infinity, and a tank gauge downstream reads
  it as full.
- flow: `ChangeFilter` no longer lets a value through *because* it rejected
  too many. SensESP's `max_skips` forces the next sample out after N
  consecutive rejections, so a stuck or glitching sender defeats exactly the
  filter meant to suppress it; a rejected value is now simply not emitted.
- test: `tools/check_public_headers.py` reads `.hpp` as well as `.h`. It globbed
  only `*.h`, so the C++ components' public headers were not exempt from the C
  ABI rules but **invisible** to the check, and the summary line reported a
  header count that silently excluded them — 48 where the tree has 67. The four
  new C++ components are declared `CPP_ONLY` like the three that were already
  there, which surfaces three Kconfig-in-a-public-header warnings that had been
  hidden.
- test: `test/host/run_all.sh` takes a per-user lock, so two concurrent runs
  can no longer rebuild and delete each other's test binaries and report
  failures that are not in the code. It is a different lock from the firmware
  build's — a host run and a build need not exclude each other.
- test: the REST harness no longer assumes the last `/signalk` request in the
  mock server's log is its own. The firmware probes that path itself to settle
  `sk.scheme`, deliberately without a token, so the Bearer-header assertion
  could read the probe instead of the request under test.

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
