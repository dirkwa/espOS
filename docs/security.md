# Security notes

## Threat model for M1

The REST API is unauthenticated, like SensESP's config UI. Anyone on the
boat network can read non-secret settings, change any setting (including
`httpd.port`), reboot the device or factory-reset it. Secrets are never
returned by the API. Authentication is a decision for later milestones
(candidates: local admin password on the device, or piggy-backing on the
SignalK token).

What *is* in place: every state-changing request must carry
`Content-Type: application/json`. A web page the user happens to have open
cannot forge such a request cross-origin without a CORS preflight, which
the device never answers — so drive-by reboots/resets from a malicious
site are blocked even without auth. Direct network access is not.

## Transport

Traffic to the SignalK server is plain `http`/`ws` unless the firmware is
built with `CONFIG_ESPOS_SK_TLS` and `sk.tls` is turned on (docs/signalk.md).
The consequence to be clear about: the access token travels in an
`Authorization` header over an unencrypted connection, so anyone who can
capture traffic on the boat LAN can replay it against the server with
whatever permissions the token was granted.

That is an acceptable trade on a boat's own network — the same network
already carries unauthenticated NMEA — and a poor one on a shared marina
network. Firmware updates do not depend on it either way: images are
signature-verified by the running app, so a plain-http image source cannot
be substituted (see below).

The device's own web server is http-only. The REST API is unauthenticated
(above), so adding TLS there without authentication would encrypt an open
door; both belong in the same future change.

## Secrets at rest

WiFi passwords (M2) and the SignalK token (M3) live in NVS. For production
builds enable flash encryption; IDF then defaults `CONFIG_NVS_ENCRYPTION=y`
and encrypts the `nvs` partition transparently using keys in the
`nvs_keys` partition (already in `partitions.csv`, flagged `encrypted`),
or the HMAC peripheral on chips that have one.

Suggested release overlay (`sdkconfig.defaults.release`, not committed
yet):

```
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_NVS_ENCRYPTION=y
```

Development boards stay unencrypted so `idf.py flash` keeps working
without burning eFuses. Note: `nvs_flash` on the linux host target does not
support encryption (host tests always run unencrypted).

## Request handling

* Request bodies are capped (`CONFIG_ESPOS_HTTPD_MAX_BODY`, default 16 KiB;
  `413` beyond that).
* All JSON input is validated against the descriptor before any write.
* Config values are validated on *read* too, so a hostile or stale NVS
  content cannot push out-of-range values into the application.

## Firmware updates (M6)

Every app image is signed (RSA-3072, `SECURE_SIGNED_APPS_NO_SECURE_BOOT`)
and the running firmware verifies the signature of any update it writes
against its compiled-in public key, so a device on the network only takes
firmware from whoever holds `secure_boot_signing_key.pem` — regardless of
whether the image came over `http://` or `https://`. This does **not**
stop someone with the USB port (no hardware Secure Boot, no flash
encryption); those remain release-overlay options. Rollback protection is
the bootloader's `APP_ROLLBACK_ENABLE` plus the confirm-on-network policy
in `espos_ota` (docs/ota.md). Keep the signing key out of the repository:
it is git-ignored, and a missing key yields a *development* key with a
loud CMake warning.
