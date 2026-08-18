# OTA (`espos_ota`) — signed updates with rollback — M6

## What happens

1. **Source.** Either a direct URL (`POST /api/v1/ota {"url": …}`, the OTA
   page's "From a URL") or a **version manifest** (`ota.manifest_url` +
   `ota.channel`), fetched ~20 s after WiFi comes up and then every
   `ota.check_h` hours when `ota.auto_check` is on. A newer matching build
   shows up as *available*; with `ota.auto_install` it is installed right
   away, otherwise someone clicks *Install*.
2. **Download + verify.** `esp_https_ota` streams the image into the other
   OTA slot. Before writing, the image header is checked: it must be an
   ESP-IDF app whose `project_name` equals the running one (an `espos`
   device does not accept a `myotherapp` image). At the end the image's
   SHA-256 **and its RSA-3072 signature** are verified
   (`CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`); the public key is
   compiled into the running firmware. Anything else is rejected with
   `image rejected: bad signature or corrupt`, and nothing changes.
3. **Reboot into the new slot** ~1.5 s after the status says `ready`.
4. **Confirm or roll back.** The new image boots as `pending_verify`
   (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`). It marks itself valid as soon
   as WiFi is connected (or, if no station is configured/enabled, after
   60 s of uptime). If it panics before that, the bootloader boots the
   previous slot on the next reset. If it runs but cannot reach the network
   within `ota.confirm_tmo_s` (default 600 s), it marks itself invalid and
   reboots into the previous slot. `POST /api/v1/ota/confirm` confirms by
   hand, `POST /api/v1/ota/rollback` goes back on purpose. The status shows
   `running.rolled_back` when the other slot holds an image that failed.

Plain `http://` sources are allowed (`CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP`):
integrity and authenticity come from the signature, not the transport.
`https://` uses the ESP-IDF certificate bundle; `ota.allow_insecure` skips
the certificate check for self-signed boat servers.

## Signing key

`sdkconfig.defaults` turns on *Require signed app images* with the RSA
scheme and signs every build with `secure_boot_signing_key.pem` in the
project root. That file is **git-ignored and never committed**. When it is
missing the root `CMakeLists.txt` generates a *development* key
(`espsecure generate_signing_key --version 2 --scheme rsa3072`) with a
warning, so a fresh checkout builds — but a device flashed with a
dev-key build only accepts updates signed with that same dev key.

For anything you ship:

```sh
espsecure generate_signing_key --version 2 --scheme rsa3072 secure_boot_signing_key.pem
# keep it outside the repo (password manager, CI secret) and copy it in for release builds
```

Every device that should take your updates must have been flashed at least
once (USB) with a build carrying that key's public part. Rotating the key
means one signed update built with the *old* key that already contains the
*new* public key. ESP32 (original) needs chip rev ≥ 3 for the RSA scheme
(`sdkconfig.defaults.esp32` pins `ESP32_REV_MIN_3`). This is *signed apps
without Secure Boot*: it protects against network-side tampering, not
against someone with the USB port. Hardware Secure Boot / flash encryption
are release-overlay decisions ([security.md](security.md)).

## Manifest format (`schema: 1`)

```json
{
  "schema": 1,
  "app": "espos",
  "builds": [
    {"version": "0.6.1", "target": "esp32p4", "channel": "stable",
     "url": "espos-esp32p4-0.6.1.bin", "size": 1314816,
     "sha256": "…hex…", "notes": "M6", "date": "2026-08-18"},
    {"version": "0.7.0-beta.1", "target": "esp32p4", "channel": "beta",
     "url": "https://fw.example.org/espos/espos-esp32p4-0.7.0-beta.1.bin"}
  ]
}
```

* `app` (optional, top level or per build) must equal the device's
  project name when present.
* Per build: `version`, `target` (`esp32 esp32s3 esp32c3 esp32c6 esp32p4`),
  `url` (absolute, or relative to the manifest URL) are required;
  `channel` defaults to `stable`; `size`, `sha256`, `notes`, `date` are
  informational (`sha256` is not what protects the image — the signature
  is).
* The device takes the **highest version** among builds matching its
  target, channel and app, and calls it *available* when it is strictly
  newer than the running version. Versions compare as dotted numbers with
  a `-prerelease` suffix sorting below the release (`1.2.10 > 1.2.9`,
  `1.0.0-beta.1 < 1.0.0`); anything unparsable compares as a plain string.
  The running version is `version.txt` in the project root (`PROJECT_VER`).
* Content type does not matter; the manifest must be ≤ 16 KiB.

A boat-side distribution service only has to serve this file and the
images (static hosting is enough) — that is the deferred "firmware
distribution service" of the plan, and nothing in espOS ties it to a host.

## Configuration (`ota` namespace)

`manifest_url`, `channel` (stable), `auto_check` (on), `check_h` (24),
`auto_install` (off), `allow_insecure` (off), `confirm_tmo_s` (600).

## API

`GET /api/v1/ota/status`, `POST /api/v1/ota/check`, `POST /api/v1/ota
{"url"?}`, `POST /api/v1/ota/confirm`, `POST /api/v1/ota/rollback`, SSE
`ota` — see [api.md](api.md).

## Testing

* Host: `test/host/espos_ota_test` (manifest selection, version compare,
  URL resolution) and `run_test.py OtaTests OtaRollbackTimeoutTests`
  (sim port: manifest check, install with progress, rejected images —
  wrong project / bad signature / not an image / 404 —, busy handling,
  confirm-once-connected, rollback after the timeout, manual rollback).
* Device (done on the ESP32-P4 panel, 2026-08-18): flash 0.6.0 by USB;
  serve `build/espos.bin` of a 0.6.1 build plus a manifest with
  `python3 -m http.server`; set `ota.manifest_url`; check → available →
  install → reboot into `ota_1` → `new image confirmed (network up)`.
  `build/espos-unsigned.bin` from the same build → `image rejected`. A
  build with `idf.py -DESPOS_BROKEN_BUILD=1 build` (aborts in `app_main`)
  → installed → boots, aborts, the bootloader boots the previous slot →
  status `rolled_back: true, other_version: 0.6.2`.
