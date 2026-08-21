# espOS REST API — v1

Base path: `/api/v1`. All request and response bodies are JSON
(`Content-Type: application/json`) unless stated otherwise. The UI (M5) is
developed against this document, so **changing anything here is a
cross-component decision** — raise it before editing.

Status of each endpoint: **M1** = implemented now; later milestones are listed
so the shape is agreed early and marked *planned*.

## Conventions

* Errors are always
  ```json
  {"error": "<machine_code>", "message": "<human text>"}
  ```
  with an appropriate 4xx/5xx status — including the server's own errors
  (`404 not_found`, `405 method_not_allowed`, `400 bad_request`, `408
  timeout`, `411 length_required`, `413 too_large`, `414 uri_too_long`, `431
  headers_too_large`, `500 internal`). Validation errors on `PUT /config`
  add a `path` (see below).
* While a reboot or factory reset is pending (≈500 ms) writes are refused
  with `503 restarting`.
* **State-changing requests (`PUT`, `POST`) must send `Content-Type:
  application/json`**, otherwise `415 unsupported_media_type`. This is the
  CSRF guard: a browser cannot send that header cross-origin without a CORS
  preflight, which the device never grants. `curl -X POST -H
  'Content-Type: application/json' …` for the system endpoints.
* Responses that must not be cached carry `Cache-Control: no-store`.
* Secrets (descriptor `secret: true`) are never returned. They read back as
  the sentinel `"********"` when set and `""` when unset; writing the
  sentinel is a no-op, so an exported document can be imported unchanged.
* Blobs are base64 strings (RFC 4648, padded).
* There is no authentication in M1. The API is reachable by anyone on the
  network; treat it like the SensESP config UI. Auth is a later decision.

## Configuration

### `GET /config` — M1

Effective configuration (stored values, else compiled-in defaults), one
object per NVS namespace:

```json
{
  "app":   {"label": "espOS device", "enabled": true, "interval_ms": 1000, ...},
  "httpd": {"port": 80}
}
```

Query: `?ns=<namespace>` limits the response to that namespace
(`404 unknown_namespace` if it does not exist or is longer than a namespace
can be; `414 uri_too_long` for a query string ≥ 128 bytes).

### `PUT /config` — M1

Body: same shape as `GET`, **partial documents allowed**. Semantics:

| Value in body                    | Effect                                    |
|----------------------------------|-------------------------------------------|
| key present with a value         | validated, then written                   |
| key present with `null`          | reset to the compiled-in default          |
| key absent                       | untouched                                 |
| secret key set to `"********"`   | untouched                                 |
| unknown namespace or key         | `400`, nothing written                    |

The whole document is validated first; on any validation error **nothing is
written**. (A storage failure while applying an already-validated document
is reported as `500 write_failed`; keys before the failing one may have been
written.)

Success `200`:
```json
{"changed": ["app.interval_ms", "httpd.port"], "restart_required": true}
```
`changed` lists keys whose *effective* value changed (writing the current
value is not a change). `restart_required` is true if any changed key is
flagged `restart_required` in its descriptor.

Failure `400`:
```json
{"error": "validation", "path": "app.interval_ms", "message": "out of range [100,60000]"}
```
`path` is `"<ns>.<key>"`, `"<ns>"`, or `""` for document-level errors
(`"malformed JSON"`, `"expected object of namespaces"`).

Other statuses: `413 too_large` (body over `CONFIG_ESPOS_HTTPD_MAX_BODY`,
default 16 KiB), `408 timeout` (body did not arrive), `503 restarting`,
`500 write_failed`. Trailing non-whitespace after the JSON document is
`400 validation` (`"malformed JSON"`).

### `GET /config/schema` — M1

JSON Schema (draft 2020-12) of the whole configuration document, generated
at build time from the config descriptors. `Content-Type:
application/schema+json`. Sent with an `ETag`; a request with a matching
`If-None-Match` gets `304`. Vendor extensions used by the UI:

| Extension                   | On          | Meaning                                  |
|-----------------------------|-------------|------------------------------------------|
| `x-espos-version`           | namespace   | descriptor schema version                |
| `x-espos-secret: true`      | key         | write-only secret (also `writeOnly`, `format: password`) |
| `x-espos-restartRequired`   | key         | takes effect after reboot                |
| `x-espos-unit`              | key         | display unit                             |
| `x-espos-type: "blob"`      | key         | base64 bytes; `x-espos-maxBytes` is the decoded limit |

## System

### `GET /system/info` — M1

```json
{
  "app": "espos", "version": "0.1.0-3-gabc1234", "idf_version": "v6.0.2",
  "chip": "esp32c6", "chip_revision": 1, "cores": 1,
  "uptime_s": 42, "free_heap": 210000, "min_free_heap": 190000,
  "reset_reason": "poweron", "config_storage_reset": false,
  "schema_etag": "6acfba355e183b19", "ui_storage": true
}
```
`ui_storage` (M5) is true when the LittleFS UI partition is mounted.
`config_storage_reset` is true when the NVS partition had to be erased at
boot (corrupt/incompatible) and every value is a default.
`reset_reason` ∈ `poweron external software panic int_wdt task_wdt wdt
deepsleep brownout sdio usb jtag efuse power_glitch cpu_lockup unknown`.

### `POST /system/reboot` — M1

`202 {"status": "rebooting"}` — the device restarts ~500 ms after
responding.

### `GET /system/coredump` — M5

Summary of the core dump saved by the last panic, `404 not_found` when
there is none:

```json
{"present": true, "size": 18848, "valid": true, "task": "httpd", "pc": "0x4003530c",
 "app_elf_sha256": "adef2c527", "version": 1179908,
 "mcause": 7, "mtval": "0x00000010", "ra": "0x40035308", "sp": "0x4ff45580", "stackdump_bytes": 448}
```
RISC-V targets carry `mcause/mtval/ra/sp/stackdump_bytes`; Xtensa targets
`exc_cause`, `exc_vaddr`, `backtrace` (array of PCs) and
`backtrace_corrupted`. `app_elf_sha256` identifies the build that crashed
(compare with `idf.py` output / `esp_app_desc`).

`GET /system/coredump/raw` streams the image as `application/octet-stream`
for `espcoredump.py --chip <target> info_corefile -c coredump.bin -t raw
build/espos.elf` (needs the ELF of exactly that build). `DELETE
/system/coredump` erases it (`{"status": "erased"}`).

`POST /system/crash` (only with `CONFIG_ESPOS_HTTPD_DEBUG_CRASH=y`, off by
default) panics on purpose to test the path.

### `POST /system/factory-reset` — M1

Arms the reboot (further writes get `503`), erases the whole NVS partition,
responds `202 {"status": "factory_reset", "rebooting": true}`, then reboots.
WiFi credentials and the SignalK token live in NVS too, so this returns the
device to provisioning.

## Logs — M5

### `GET /logs`

The in-RAM log ring (`CONFIG_ESPOS_LOG_RING_SIZE`, 16 KiB), paged by
sequence number: `?after=<seq>` returns lines with a higher sequence
(default: from the oldest kept), `?limit=<n>` (default 200, max 1000).

```json
{"first": 1, "next": 101, "dropped": 0, "size": 16384, "used": 5911, "gap": false, "from": 1,
 "lines": ["I (500) espos_config: ready: 4 namespace(s), schema 8851b3f24a88fe45", "…"]}
```
`lines[i]` has sequence `from + i`; poll on with `after = next - 1`.
`gap` is true when `after` was older than the ring still holds (lines were
overwritten in between); `first`/`dropped` say how many. Lines are the
console lines without colour codes, truncated at
`CONFIG_ESPOS_LOG_LINE_MAX` (256). Streamed in chunks, so a full ring never
has to fit in RAM twice.

### `PUT /logs/level`

`{"level": "debug", "tag": "espos_sk"}` (`tag` optional, default `*`;
`level` ∈ `none error warn info debug verbose`) → `200 {"tag","level"}`;
`400 validation` otherwise. Runtime only (`esp_log_level_set`), not
persisted.

## Static UI — M5

Everything that is not `/api/…` is served from the LittleFS `storage`
partition (the gzipped Vite bundle, see [ui.md](ui.md)): `<path>.gz` is
preferred and sent with `Content-Encoding: gzip` (`curl --compressed`),
`/assets/*` (content-hashed) with `Cache-Control: public, max-age=31536000,
immutable`, everything else `no-cache`. An extension-less path that does
not exist falls back to `/index.html` (SPA routes such as `/wifi`); a
missing file with an extension is `404 {"error": "not_found"}`, as is
anything unknown under `/api/`. When the partition has no `index.html` the
placeholder page embedded in the firmware is served instead
(`ui_storage: false` in `/system/info`).

## WiFi

### `GET /wifi/status` — M2

The status document described in [wifi.md](wifi.md): `state` ∈
`disabled unconfigured connecting obtaining_ip connected backoff`,
`reason: {code, text}`, link/IP details, `backoff_ms` while backing off,
counters, `portal: {active, ssid, ip?, clients?}`.

### `POST /wifi/scan` — M2

Starts an asynchronous scan. `202 {"status": "scanning"}`; `409 busy` if
the driver cannot scan right now. Requires the JSON content type.

### `GET /wifi/scan` — M2

```json
{"scanning": false, "age_s": 3,
 "results": [{"ssid": "Boat", "bssid": "aa:bb:cc:dd:ee:ff", "rssi": -52, "channel": 6, "auth": "wpa2/wpa3"}]}
```
`age_s` is `null` before the first scan. `auth` ∈ `open wep wpa wpa2
wpa/wpa2 wpa2-enterprise wpa3 wpa2/wpa3 wapi owe other`. Results are cached;
a `wifi_scan` SSE event carries the same document when a scan finishes.

### Captive-portal probes — M2

`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
`/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`,
`/canonical.html`, `/success.txt` answer `302 → http://192.168.4.1/`.

## Events

### `GET /events` — M2

`text/event-stream` (chunked, `retry: 3000` first, a `: ping` comment every
15 s). Events:

| event       | data                                | when                                   |
|-------------|-------------------------------------|----------------------------------------|
| `wifi`      | the `/wifi/status` document          | on connect (snapshot) and every change |
| `wifi_scan` | the `/wifi/scan` document            | scan finished                          |
| `config`    | `{"ns": "...", "key": "..."}`         | a key's effective value changed        |

| `sk`        | the `/sk/status` document            | on connect and every token/server change |
| `sk_servers`| the `/sk/servers` document           | on connect and after each discovery pass |
| `sk_ws`     | the `ws` object of `/sk/status`      | stream connect/disconnect, error, drops |
| `logs`      | `{"next": <seq>}`                     | at most every 500 ms when new log lines arrived; fetch `/logs?after=` |
| `ota`       | the `/ota/status` document           | on connect, state changes, every ~32 KiB of download |
| `ble`       | the `/ble/status` document           | on connect (snapshot)                  |

At most `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS` (3) streams; when full the
oldest stream is evicted (clients reconnect via `retry`).

## SignalK

### `GET /sk/status` — M3

```json
{
  "token": {"state": "approved", "has_token": true, "busy": false,
            "approved_s": 120, "next_action_s": 40, "last_check_s": 20,
            "last_http_status": 200, "last_error": "",
            "counts": {"requests": 1, "approved": 1, "denied": 0, "unauthorized": 0}},
  "server": {"host": "192.168.1.10", "port": 80, "self": "urn:mrn:signalk:uuid:…",
             "source": "discovered", "name": "boat", "swname": "signalk-server", "swvers": "2.31.1"},
  "client_id": "…uuid…", "description": "espOS espos-1a2b", "permissions": "readwrite",
  "discovery": {"enabled": true, "count": 2, "last_s": 12},
  "ws": {"enabled": true, "connected": true, "connected_s": 300, "reconnects": 1,
         "sent": 1234, "send_errors": 0, "pending": 0,
         "buffered": 0, "buffered_bytes": 0, "dropped": 0, "last_error": "",
         "meta": {"declared": 8, "reconciled": 8},
         "in": {"subs": 2, "frames": 590, "received": 586},
         "put": {"pending": 0, "ok": 3, "failed": 1}}
}
```
`token.state` ∈ `no_server requesting pending verifying approved denied
open error`; `pending_href`/`pending_s` while pending; `server.source` ∈
`discovered manual none`. The token itself is never returned.

`ws` (M4) is the delta stream: `pending` = values in the open batching
window, `buffered`/`buffered_bytes` = messages held in the offline ring,
`dropped` = messages the ring had to discard (oldest first),
`next_retry_s` while disconnected, `meta` = declared / reconciled counts.
`in` (M7): active subscriptions, text frames read, value/meta items
delivered; `put` (M7): requests in flight, answered OK, failed/timed out.

### `GET /sk/servers` — M3

`{"servers": [{"host","port","self","name","roles","swname","swvers","seen_s","selected"}], "last_s": 12}`

### `POST /sk/discover`, `POST /sk/request`, `POST /sk/forget` — M3

`202` with a status word; JSON content type required. `request` re-requests
access (from `denied`/`error`/`open`); `forget` drops the token (a pending
request keeps being polled).

### `POST /sk/token` — M3

Body `{"token": "<jwt>"}` → `202 {"status": "verifying"}`; the token is
verified against `/signalk/v1/api/self` and kept if it works. `400
validation` for a bad body.

### `POST /sk/publish` — M4

Publish a value for a `vessels.self` path — the C publish API over HTTP,
for scripts and the setup page:

```json
{"path": "espos.demo.count", "value": 42,
 "meta": {"units": "1", "description": "demo"}, "period_ms": 1000}
```
`meta`/`period_ms` are optional and go through `espos_sk_declare_meta()`
(same rules: non-standard paths only, `period_ms` adds `timeout`). `202
{"status":"queued"}`; `400 validation` for a bad body or an over-long
path/value; `503 not_ready` before the SignalK component is up. Queued
does not mean delivered: the value goes into the current batching window
and, if the stream is down, into the offline ring (see `ws` in
`/sk/status`).

### `POST /sk/put`, `GET /sk/put` — M7

`{"path": "navigation.anchor.maxRadius", "value": 30}` → `202
{"status":"sent"}` (`503 not_connected` without a stream, `429 busy` with
8 in flight, `400 validation`). The server answers asynchronously; `GET
/sk/put` returns the last answer `{"request_id","state","status_code",
"message"}` or `null`.

SSE events: `sk` (the status document, on connect and on change),
`sk_servers` (the servers document after each discovery pass), `sk_ws`
(the `ws` object on every stream state change).

## OTA — M6

### `GET /ota/status`

```json
{"state": "idle", "last_error": "",
 "running": {"version": "0.6.1", "project": "espos", "target": "esp32p4", "slot": "ota_1",
             "image_state": "valid", "pending_verify": false, "confirmed": true,
             "other_slot": "ota_0", "other_version": "0.6.0", "rolled_back": false,
             "built": "Aug 18 2026 14:20:25", "idf": "v6.0.2"},
 "manifest": {"url": "http://…/manifest.json", "channel": "stable", "auto_check": true,
              "auto_install": false, "last_check_s": 120, "next_check_s": 86280},
 "progress": {"received": 0, "total": 0},
 "available": {"version": "0.6.2", "url": "http://…/espos-esp32p4-0.6.2.bin", "size": 0,
               "sha256": "", "notes": "…", "newer": true}}
```
`state` ∈ `idle checking available downloading verifying ready failed`
(`ready` = installed, rebooting in ~1.5 s). `image_state` ∈ `valid
pending_verify new invalid aborted undefined`; `pending_verify` is true
while a fresh image has not confirmed itself; `rolled_back` when the other
slot holds an image that failed. `available` is `null` until a manifest
check found something. `last_check_s`/`next_check_s` are `null` before the
first check / when auto-check is off.

### `POST /ota/check`, `POST /ota`, `POST /ota/confirm`, `POST /ota/rollback`

JSON content type required; `202 {"status": …}`. `POST /ota` with
`{"url": "http(s)://…"}` installs that image, with `{}` the *available*
build (`404 not_found` if none). `409 busy` while a check or install is
running; `400 validation` for a non-http(s) URL. Progress and the outcome
arrive via `GET /ota/status` and the `ota` SSE event.

## BLE gateway

Present only when `espos_ble` is in the build and Bluetooth is enabled in
sdkconfig. The component bridges BLE devices to signalk-server's BLE provider
API; it decodes nothing itself, so there is no device or sensor model here —
what a device *is* remains a server-side concern.

### `GET /ble/status`

```json
{
  "enabled": true, "scanning": true, "mac": "D8:85:AC:FA:2C:46",
  "scan_hits": 2766, "adv_received": 2766, "adv_posted": 1840,
  "adv_dropped": 0, "adv_pending": 126,
  "post_success": 46, "post_fail": 0,
  "ws_connected": true,
  "gatt_sessions": 0, "gatt_max": 3
}
```

* `scan_hits` counts advertisements seen by the radio, `adv_received` those
  handed to the gateway; a gap between them means the intake callback is being
  starved.
* `adv_pending` is the ring buffer's depth and `adv_dropped` the running total
  shed once it is full (oldest first). Sustained growth in `adv_dropped` means
  the POST interval or the buffer is too small for the local radio traffic —
  it is a real count, not an estimate.
* `post_fail` counts POSTs the server refused or that never completed; a 401
  or 403 is reported to `espos_sk`, which owns the token, rather than
  triggering a second access request from here.
* `gatt_max` is the concurrent-session ceiling (3, Bluedroid's own limit) and
  is what the gateway advertises to the server in its `hello`.

There is no `POST /ble/...`: scanning is driven by config
(`PUT /config` with the `ble` namespace) and GATT sessions are opened by the
server over the control WebSocket, never through this API.

## Planned (shape reserved, not implemented)

Nothing — M1–M6 are implemented. Future additions go here first.
