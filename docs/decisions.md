# Decisions and additions vs. the plan

espOS was built against a written plan that lives outside this repository
(the section numbers below refer to it). Every deviation from that plan and
every addition to it is recorded here so nothing is silently applied
(plan §7). Moved from the README unchanged when the milestone table there
was retired.

* **ESP-IDF 6.0.2** instead of the plan's original 5.x (owner decision,
  2026-08-18). Consequence: IDF 6 removed the bundled cJSON, so
  `espressif/cjson` (Espressif-maintained, exact-pinned) is the single
  registry dependency.
* Beyond the M1 bullet list, and marked M1 in [rest-api.md](rest-api.md):
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
  (ota.md). Plain-http image sources are allowed for the same reason.
  ESP32 (original) builds pin chip rev ≥ 3 for the RSA scheme.
* **2026-09-07: the C ABI is the stable contract.** Plain C headers under
  `components/*/include` — `extern "C"`, `esp_err_t`, fixed-width ints,
  callback + `void *arg`, no IDF header but `esp_err.h` (two frozen
  exceptions), no `CONFIG_` in new headers, `ESPOS_ABI_VERSION` — are what
  every consumer gets; rules and the CI check are in
  [development.md](development.md) "Public API rules". A C++ facade is
  header-only sugar above it (`espos_n2k`/`espos_voice`/`espos_audio` stay
  C++-only contracts until wrapped). Rust not now: no released esp-idf-sys for
  IDF 6, Tier-3 targets, no P4 hosted-WiFi host, no esp-sr/LVGL path — the ABI
  is shaped so `bindgen` yields an `espos-sys` later without a rewrite (plan
  §3.4, revisit triggers there).
- 2026-09-07: `espressif/mdns` is espos_wifi's dependency and a public `REQUIRES` (espos_sk browses through it); the responder is brought up from `espos_wifi_start()` on the caller's task, never from an event handler (mdns 1.11.3 hostname/service calls block on the responder task).
