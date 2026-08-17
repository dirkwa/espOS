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
