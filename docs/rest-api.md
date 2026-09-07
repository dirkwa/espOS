# espOS REST API — v1

Base path: `/api/v1`. All request and response bodies are JSON
(`Content-Type: application/json`) unless stated otherwise. The UI (M5) is
developed against this document, so **changing anything here is a
cross-component decision** — raise it before editing.

Status of each endpoint: **M1** = implemented now; later milestones are listed
so the shape is agreed early and marked *planned*. Each endpoint is also marked
**public** or **protected** — whether [authentication](#authentication) applies
once an API key is set.

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
  'Content-Type: application/json' …` for the system endpoints. A request
  authenticated by the login *cookie* must in addition come from the device's
  own origin (see Authentication).
* Responses that must not be cached carry `Cache-Control: no-store`.
* Secrets (descriptor `secret: true`) are never returned. They read back as
  the sentinel `"********"` when set and `""` when unset; writing the
  sentinel is a no-op, so an exported document can be imported unchanged.
* Blobs are base64 strings (RFC 4648, padded).
* **Authentication** is the section below. With `httpd.api_key` unset the
  API is open to anyone on the network (the default — treat it like the
  SensESP config UI); once a key is set, every endpoint marked *protected*
  needs `Authorization: Bearer <key>` or the login cookie, else `401`.

## Authentication

Everything is **protected** unless marked **public**. Protection is off until
`httpd.api_key` is set (so an update keeps an existing device open); from then
on a protected request must carry one of

* `Authorization: Bearer <key>` — machine clients: the designer, a fleet
  plugin, scripts. Stateless.
* the `espos_sid` cookie from `POST /auth/login` — browsers. `fetch()` and
  `EventSource` send it by themselves; `HttpOnly; SameSite=Strict; Path=/`.

or arrive on the device's own setup-portal network, which is exempt (the
lockout recovery, [security.md](security.md)). Otherwise:

| Status | `error` | When |
|---|---|---|
| `401` | `unauthorized` | no or invalid credential, a stale cookie included; carries `WWW-Authenticate: Bearer realm="espOS"` |
| `403` | `forbidden` | a cookie-authenticated `PUT`/`POST`/`DELETE` whose `Origin` (else `Referer`) host is not the `Host`, or that has neither — Bearer requests skip this |
| `403` | `auth_unconfigured` | the build has `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED=y` and no key is set yet; set one from the portal |
| `429` | `too_many_attempts` | five wrong keys within 60 s: every key check (login and Bearer, the right key too) answers this for 30 s, `Retry-After` says how long; live cookies keep working |

The check runs before the handler, for every endpoint registered through
`espos_httpd_register()` — an application's own included; an endpoint that
must stay open registers with `espos_httpd_register_ex(uri, ESPOS_HTTPD_PUBLIC)`.
Changing or clearing the key drops every session. Over plain http the key
crosses the network in clear, like the SignalK token does.

### `POST /auth/login` — public

Body `{"key": "<httpd.api_key>"}` (JSON content type required) → `204` with

```
Set-Cookie: espos_sid=<32 hex>; HttpOnly; SameSite=Strict; Path=/; Max-Age=<httpd.session_ttl_s>
```

`401 unauthorized` for a wrong key (counted), `429` while throttled, `409
auth_open` when no key is configured, `400 validation` for a bad body.
Sessions live in RAM: `CONFIG_ESPOS_HTTPD_MAX_SESSIONS` (4) at a time — one
more evicts the session idle longest — and a reboot ends them all.

### `POST /auth/logout` — public

Drops the session the cookie names, if any → `204` with a cookie of
`Max-Age=0`. JSON content type required.

### `GET /auth/status` — public

```json
{"required": true, "configured": true, "authenticated": true, "method": "cookie"}
```

`required`: protected endpoints need a credential (a key is set, or the build
requires one); `configured`: a key is set; `authenticated`: *this* request
carried a valid one, or came from the portal; `method` ∈ `none bearer cookie
portal`. The web UI decides from this whether to show its login page. A wrong
Bearer on this endpoint counts toward the throttle like any other key check.

## Configuration

### `GET /config` — M1 · protected

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

### `PUT /config` — M1 · protected

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

### `GET /config/schema` — M1 · protected

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

### `GET /system/ping` — public

```json
{"app": "espos", "version": "0.7.0-3-gabc1234", "auth": true}
```
Liveness for a fleet page or a discovery tool: which firmware, and whether it
wants a key (`auth` is `/auth/status`'s `required`). Nothing mDNS does not
already advertise.

### `GET /system/info` — M1 · protected

```json
{
  "app": "espos", "version": "0.1.0-3-gabc1234", "idf_version": "v6.0.2",
  "chip": "esp32c6", "chip_revision": 1, "cores": 1,
  "uptime_s": 42, "free_heap": 210000, "min_free_heap": 190000,
  "reset_reason": "software", "config_storage_reset": false,
  "schema_etag": "6acfba355e183b19", "ui_storage": true,
  "time": {"synced": true, "source": "sntp", "now": 1788775933456},
  "last_reset": {
    "reason": "software", "health_key": "skLinkStalled",
    "message": "stream down for over 300 s while WiFi reports connected",
    "min_free_heap_before": 148216, "min_internal_before": 31720,
    "largest_block_before": 25600, "uptime_before_s": 86742,
    "at": "2026-09-07T04:12:31Z"
  }
}
```
`ui_storage` (M5) is true when the LittleFS UI partition is mounted.
`config_storage_reset` is true when the NVS partition had to be erased at
boot (corrupt/incompatible) and every value is a default.
`reset_reason` ∈ `poweron external software panic int_wdt task_wdt wdt
deepsleep brownout sdio usb jtag efuse power_glitch cpu_lockup unknown`.

`time` is the short form of `GET /time` below and is always present: `synced`,
`source` ∈ `none rtc sk manual sntp`, and `now` in unix milliseconds (`0` when
the device has not been told the time). A firmware built without `espos_time`
reports `{"synced": false, "source": "none", "now": 0}` — the honest answer,
so a client never has to guess whether the component is there.

`last_reset` is the record the health watchdog ([health.md](health.md)) left
when it restarted the device: `reason` (the reset reason, `software` for a
watchdog restart), `health_key` and `message` of the condition that struck
out, the low-water marks of total heap (`min_free_heap_before`) and internal
RAM (`min_internal_before`), the largest free internal block at the time
(`largest_block_before`), how long that boot had run (`uptime_before_s`) and
the wall-clock time (`at`, ISO 8601 UTC; `null` when the clock was never set).
`null` when the last reset was not the watchdog's — power-on, a panic (see
`/system/coredump`), an OTA reboot, `POST /system/reboot`. It stays for the
whole boot and is gone after the next reset, whatever its cause.

### `POST /system/reboot` — M1 · protected

`202 {"status": "rebooting"}` — the device restarts ~500 ms after
responding.

### `GET /system/coredump` — M5 · protected

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

### `POST /system/factory-reset` — M1 · protected

Arms the reboot (further writes get `503`), erases the whole NVS partition,
responds `202 {"status": "factory_reset", "rebooting": true}`, then reboots.
WiFi credentials and the SignalK token live in NVS too, so this returns the
device to provisioning.

## Logs — M5

### `GET /logs` · protected

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

### `PUT /logs/level` · protected

`{"level": "debug", "tag": "espos_sk"}` (`tag` optional, default `*`;
`level` ∈ `none error warn info debug verbose`) → `200 {"tag","level"}`;
`400 validation` otherwise. Runtime only (`esp_log_level_set`), not
persisted.

## Static UI — M5 — public

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

## Network

### `GET /net/status`

The transport-neutral view of the network ([net.md](net.md)): whether a
default route exists, on which interface, with which addresses, and the
device's identity on it.

```json
{"up": true, "iface": "wifi_sta", "ip": "192.168.1.23", "netmask": "255.255.255.0", "gateway": "192.168.1.1",
 "ip6_ll": "fe80::3e71:bfff:fe12:1a2b", "mac": "3c:71:bf:12:1a:2b", "hostname": "espos-1a2b", "id": "1a2b",
 "rssi": -59, "up_count": 1, "up_s": 26}
```

`iface` ∈ `none wifi_sta eth thread` (`none` while down, with the address
strings empty); `mac` is the base MAC and `id` its last two bytes, the
device id every default name derives from; `rssi` is `null` unless the
route is the WiFi station; `up_count` counts how often the route came up or
moved since boot; `up_s` is seconds since it came up (0 when down); `ip6_ll`
is `""` when the interface has no link-local IPv6 address. `/wifi/status`
below is unchanged and keeps the WiFi-specific detail.

## Time

### `GET /time` — protected

What the device believes the time is and where it learned it ([time.md](time.md)).

```json
{"synced": true, "source": "sntp", "now": 1788775933456,
 "iso": "2026-09-07T10:12:13.456Z", "tz": "UTC0",
 "sntp": {"enabled": true, "running": true, "from_dhcp": true,
          "servers": ["pool.ntp.org"]}}
```

`source` ∈ `none rtc sk manual sntp`, ranked in that order: a lower-ranked
source never overrides a higher one, so nothing walks the clock back once NTP
has answered. `now` is unix milliseconds and `iso` the same instant as ISO 8601
UTC; both are `0` and `""` when nothing has set the clock — never a
plausible-looking 1970, so a missing time reads as missing.

`synced` is false either when no source has spoken or when the only one that
did was an RTC value carried through a deep sleep that has since gone stale
(`CONFIG_ESPOS_TIME_RTC_STALE_H`); in the stale case `now` is still the
device's best guess and `source` still says `rtc`.

`sntp.running` is true once polling started, which happens on the first
`NETWORK_UP` rather than at boot. `sntp.servers` lists the configured ones; a
server supplied by DHCP is not among them.

### `PUT /time` — protected

```json
{"now": 1788775933456}
```

Sets the clock as `source: "manual"`, and replies with the `GET /time`
document. `tz` may be sent instead of or alongside `now` to set the POSIX
timezone (`time.tz`) — a display concern only; nothing espOS publishes is ever
in local time.

`409 outranked` when a higher-ranked source already has the clock — in
practice, when SNTP has synced. `400 validation` when `now` is not a positive
unix-millisecond value or `tz` is too long.

## WiFi

### `GET /wifi/status` — M2 · protected

The status document described in [wifi.md](wifi.md): `state` ∈
`disabled unconfigured connecting obtaining_ip connected backoff`,
`reason: {code, text}`, link/IP details, `backoff_ms` while backing off,
counters, `portal: {active, ssid, ip?, clients?}`.

### `POST /wifi/scan` — M2 · protected

Starts an asynchronous scan. `202 {"status": "scanning"}`; `409 busy` if
the driver cannot scan right now. Requires the JSON content type.

### `GET /wifi/scan` — M2 · protected

```json
{"scanning": false, "age_s": 3,
 "results": [{"ssid": "Boat", "bssid": "aa:bb:cc:dd:ee:ff", "rssi": -52, "channel": 6, "auth": "wpa2/wpa3"}]}
```
`age_s` is `null` before the first scan. `auth` ∈ `open wep wpa wpa2
wpa/wpa2 wpa2-enterprise wpa3 wpa2/wpa3 wapi owe other`. Results are cached;
a `wifi_scan` SSE event carries the same document when a scan finishes.

### Captive-portal probes — M2 — public on the portal network

`/generate_204`, `/gen_204`, `/hotspot-detect.html`,
`/library/test/success.html`, `/connecttest.txt`, `/ncsi.txt`, `/redirect`,
`/canonical.html`, `/success.txt` answer `302 → http://192.168.4.1/`. They
only matter to a phone that just joined the portal, and that network is
exempt from authentication; on the station side they are ordinary protected
endpoints.

## Events

### `GET /events` — M2 · protected

`text/event-stream` (chunked, `retry: 3000` first, a `: ping` comment every
15 s). Protected like the rest: a browser's `EventSource` sends the login
cookie by itself, a script sends the Bearer header. Events:

| event       | data                                | when                                   |
|-------------|-------------------------------------|----------------------------------------|
| `net`       | the `/net/status` document           | on connect (snapshot) and every change: route up/down/moved, RSSI refresh |
| `wifi`      | the `/wifi/status` document          | on connect (snapshot) and every change |
| `wifi_scan` | the `/wifi/scan` document            | scan finished                          |
| `config`    | `{"ns": "...", "key": "..."}`         | a key's effective value changed        |

| `sk`        | the `/sk/status` document            | on connect and every token/server change |
| `sk_servers`| the `/sk/servers` document           | on connect and after each discovery pass |
| `sk_ws`     | the `ws` object of `/sk/status`      | stream connect/disconnect, error, drops |
| `sk_tls`    | the `/sk/tls` document               | on connect and whenever the pinned certificate changes |
| `logs`      | `{"next": <seq>}`                     | at most every 500 ms when new log lines arrived; fetch `/logs?after=` |
| `ota`       | the `/ota/status` document           | on connect, state changes, every ~32 KiB of download |
| `ble`       | the `/ble/status` document           | on connect (snapshot)                  |

At most `CONFIG_ESPOS_HTTPD_SSE_MAX_CLIENTS` (3) streams; when full the
oldest stream is evicted (clients reconnect via `retry`).

## SignalK

### `GET /sk/status` — M3 · protected

```json
{
  "token": {"state": "approved", "has_token": true, "busy": false,
            "approved_s": 120, "next_action_s": 40, "last_check_s": 20,
            "last_http_status": 200, "last_error": "",
            "counts": {"requests": 1, "approved": 1, "denied": 0, "unauthorized": 0,
                       "cert_errors": 0}},
  "server": {"host": "192.168.1.10", "port": 80, "self": "urn:mrn:signalk:uuid:…",
             "source": "discovered", "scheme": "http", "name": "boat",
             "swname": "signalk-server", "swvers": "2.31.1"},
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
open error cert_error`; `pending_href`/`pending_s` while pending;
`server.source` ∈ `discovered manual pinned none`; `server.scheme` ∈ `http
https` — what is actually in use, which under `sk.scheme = auto` is the only
place to read it. The token itself is never returned.

`cert_error` means the server's TLS certificate is not the one this device
trusts. The token is kept (it is the transport that is wrong, not the
credential), the retry is a flat 60 s rather than the exponential backoff an
unreachable server gets, and the stream stays down. `GET /sk/tls` says what
differed; `DELETE /sk/tls` accepts the new certificate. See
[SignalK → TLS](signalk.md#tls-https-wss).

`ws` (M4) is the delta stream: `pending` = values in the open batching
window, `buffered`/`buffered_bytes` = messages held in the offline ring,
`dropped` = messages the ring had to discard (oldest first),
`next_retry_s` while disconnected, `meta` = declared / reconciled counts.
`in` (M7): active subscriptions, text frames read, value/meta items
delivered; `put` (M7): requests in flight, answered OK, failed/timed out.

### `GET /sk/servers` — M3 · protected

`{"servers": [{"host","port","self","name","roles","swname","swvers","scheme","seen_s","selected"}], "last_s": 12}`

`scheme` is `https` for a server that advertised itself as
`_signalk-https._tcp` rather than `_signalk-http._tcp`, which is how
signalk-server says its `ssl` setting is on.

### `GET /sk/tls` — S1 · protected

Present only in a build with `CONFIG_ESPOS_SK_TLS` (the default).

```json
{
  "trust": "tofu",
  "pinned": {"kind": "leaf", "cn": "boat.local", "san": "192.168.1.10,boat.local",
             "fingerprint": "9f2c…", "since": 1789000000},
  "last_error": "",
  "presented": {"cn": "boat.local", "fingerprint": "9f2c…"}
}
```

`trust` ∈ `tofu ca bundle` (the `sk.tls_trust` setting). `pinned` is `null`
until something is anchored; `kind` ∈ `ca leaf` — `ca` binds the issuing CA
*and* the `san` set, so a renewal by the same CA for the same names is
accepted, while `leaf` is the certificate itself and a renewal needs a
`DELETE`. `fingerprint` is the SHA-256 of whichever certificate is anchored,
hex. `since` is unix seconds, `0` when the device had no clock at the time.
`presented` is what the last handshake showed (`null` before the first), and
`last_error` says how it differed — the pair is what makes a `cert_error`
actionable.

### `DELETE /sk/tls` — S1 · protected

Forget the anchor and retry at once: the deliberate "yes, that certificate
really was replaced" a pinned device needs after a renewal it could not follow
on its own. `202 {"status":"reset"}`.

### `PUT /sk/tls/ca` — S1 · protected

Body `{"pem": "-----BEGIN CERTIFICATE-----…"}` → `200
{"status":"stored","trust":"ca"}`. Validates the PEM before storing it (`400
validation` for anything that is not a certificate, or one over
`CONFIG_ESPOS_SK_TLS_CA_MAX`), writes it to `sk.ca_pem` and switches
`sk.tls_trust` to `ca`. Equivalent to setting both config keys, with the
parse check.

### `POST /sk/discover`, `POST /sk/request`, `POST /sk/forget` — M3 · protected

`202` with a status word; JSON content type required. `request` re-requests
access (from `denied`/`error`/`open`); `forget` drops the token (a pending
request keeps being polled).

### `POST /sk/token` — M3 · protected

Body `{"token": "<jwt>"}` → `202 {"status": "verifying"}`; the token is
verified against `/signalk/v1/api/self` and kept if it works. `400
validation` for a bad body.

### `POST /sk/publish` — M4 · protected

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

### `POST /sk/put`, `GET /sk/put` — M7 · protected

`{"path": "navigation.anchor.maxRadius", "value": 30}` → `202
{"status":"sent"}` (`503 not_connected` without a stream, `429 busy` with
8 in flight, `400 validation`). The server answers asynchronously; `GET
/sk/put` returns the last answer `{"request_id","state","status_code",
"message"}` or `null`.

SSE events: `sk` (the status document, on connect and on change),
`sk_servers` (the servers document after each discovery pass), `sk_ws`
(the `ws` object on every stream state change), `sk_tls` (the `/sk/tls`
document when the pinned certificate changes).

## OTA — M6

### `GET /ota/status` · protected

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

### `POST /ota/check`, `POST /ota`, `POST /ota/confirm`, `POST /ota/rollback` · protected

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

### `GET /ble/status` · protected

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

Nothing — M1–M7 and the authentication are implemented. Future additions go
here first.
