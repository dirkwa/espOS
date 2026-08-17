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
  "schema_etag": "6acfba355e183b19"
}
```
`config_storage_reset` is true when the NVS partition had to be erased at
boot (corrupt/incompatible) and every value is a default.
`reset_reason` ∈ `poweron external software panic int_wdt task_wdt wdt
deepsleep brownout sdio usb jtag efuse power_glitch cpu_lockup unknown`.

### `POST /system/reboot` — M1

`202 {"status": "rebooting"}` — the device restarts ~500 ms after
responding.

### `POST /system/factory-reset` — M1

Arms the reboot (further writes get `503`), erases the whole NVS partition,
responds `202 {"status": "factory_reset", "rebooting": true}`, then reboots.
WiFi credentials and the SignalK token live in NVS too, so this returns the
device to provisioning.

## Static UI

`GET /` and `GET /index.html` serve the UI (M1: an embedded placeholder
page; M5: the built bundle). Unknown paths under `/api/` return `404
{"error": "not_found"}`.

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
  "discovery": {"enabled": true, "count": 2, "last_s": 12}
}
```
`token.state` ∈ `no_server requesting pending verifying approved denied
open error`; `pending_href`/`pending_s` while pending; `server.source` ∈
`discovered manual none`. The token itself is never returned.

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

SSE events: `sk` (the status document, on connect and on change),
`sk_servers` (the servers document after each discovery pass).

## Planned (shape reserved, not implemented)

| Endpoint                     | Milestone | Notes                                          |
|------------------------------|-----------|------------------------------------------------|
| `GET /logs`                  | M5        | log ring buffer                                |
| `GET /system/coredump`       | M5        | last crash summary                             |
| `POST /ota`, `GET /ota/status` | M6      | update from URL / manifest                     |
