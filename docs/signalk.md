# SignalK (`espos_sk`) — discovery, access token, delta stream, inbound

M3: find the server, get and keep a token. M4: stream published values as
deltas over a WebSocket, buffer them while offline, reconcile metadata,
publish device health. M7: subscribe to paths and families, receive values
and meta, send PUT requests and raw frames — what a display or controller
needs on top of a sensor. Plus one HTTP client for everything else an
application asks the server over REST (`espos_sk_http.h`).

## Discovery

`espos_sk` browses `_signalk-http._tcp` via mDNS every `sk.discover_s`
(default 60 s, immediately when WiFi comes up) and keeps up to
`ESPOS_SK_MAX_SERVERS` (12) servers with their TXT records (`self`, `roles`,
`swname`, `swvers`). Entries that
drop out of one query survive two intervals (mDNS is lossy).
`GET /api/v1/sk/servers` lists them; `sk_servers` SSE events fire after
every pass.

The responder the queries go through — and the device's own
`<hostname>.local`, `_http._tcp` and `_espos._tcp` records — is
`espos_wifi`'s ([wifi.md](wifi.md), "mDNS"); `espos_sk` only browses. A pass
waits for `espos_mdns_is_ready()` (the station reports connected a few
milliseconds before the responder's `ESPOS_EVENT_MDNS_READY` reaches it,
and an empty first pass would only be retried a whole interval later) and
returns nothing without a link. Built with `CONFIG_ESPOS_WIFI_MDNS=n` there
is no responder to browse with: discovery is off and `sk.server_host` must
be set.

Which server is used:

| `sk` config                          | choice                                             |
|--------------------------------------|----------------------------------------------------|
| `server_host` set                    | that host:port (manual, for networks without mDNS) |
| `server_self` set                    | the discovered server with that self URN           |
| neither                              | sticky: the server our token / pending request belongs to; else the discovered `master` with the lowest self URN; else any |

Discovered servers that turn out unreachable (wrong subnet, gone) are
skipped for five minutes so one dead entry cannot block the machine.

## Token state machine

`sk_token_sm.c` is pure C over an injected port (HTTP calls, storage,
timer, clock) and is unit-tested for every transition
(`test/host/espos_sk_test`). Verified against a real signalk-server 2.31
(the flow, status codes and body shapes below are what it actually
returns).

```
NO_SERVER ──server known──▶ evaluate:
   pending href for this server? ──▶ REQUESTED (resume polling)
   stored token for this self?  ──▶ VERIFYING ── 200 ▶ APPROVED
   else                          ──▶ IDLE: POST /signalk/v1/access/requests
                                            {clientId, description, permissions}
        202 {state:PENDING, href}   ▶ REQUESTED, href persisted
        404 (security disabled)     ▶ OPEN  (no token needed; re-POSTed every 60 s —
                                            GET /self is blind to security when allow_readonly is on)
        403 (device requests off)   ▶ DENIED
        400 "already requested"     ▶ ERROR, retry in 60 s
        unreachable / 5xx           ▶ ERROR, backoff 10 s → 5 min
REQUESTED: GET href every 5 s, ×1.5 up to 60 s
        state PENDING               ▶ keep polling
        COMPLETED + APPROVED + token▶ token persisted (keyed by self) ▶ VERIFYING
        COMPLETED + DENIED          ▶ DENIED (no auto retry; UI offers "request again")
        404 / 500 "not found"       ▶ server lost it ▶ IDLE (request again)
VERIFYING / APPROVED: GET /signalk/v1/api/self with Bearer
        200                         ▶ APPROVED (self URN learned/updated), re-check every check_s
        401 / 403                   ▶ token dropped ▶ IDLE (request again)
APPROVED + any other SK call reporting 401/403 (espos_sk_report_unauthorized) ▶ IDLE
```

Design points from the plan, all implemented:

* **`clientId` is a v4 UUID generated once** and stored in the `skstate`
  NVS namespace, never in the exported configuration; it survives config
  import/export and factory-reset only wipes it because the whole partition
  goes.
* **Tokens are keyed by the server's `self` URN.** A server that changes
  address keeps its token (discovery re-resolves the host by self); a
  reinstalled server (new self) gets a fresh request; a token learned for a
  manual host without mDNS has its self filled in from the first successful
  verify.
* **A pending `href` is persisted** with the server it belongs to; a reboot
  mid-approval resumes polling instead of creating a duplicate request.
* **Manual token paste**: `POST /api/v1/sk/token {"token": "…"}` → verified
  immediately.
* Secrets: the token never appears in any API response or SSE event; the
  store lives in the same (optionally encrypted) NVS partition as the config.

## Delta stream (M4)

`espos_sk_publish_number/string/bool/json(path, value)` is the whole app
API: thread-safe, never blocks, works before WiFi is up. Values are for
`vessels.self`; the source label is `espos.<hostname>`.

Pipeline (`sk_delta.c`, pure C, unit-tested; `sk_ws.c` = the transport
task):

1. **Batching window** (`sk.batch_ms`, default 100 ms): everything
   published inside one window becomes one delta message with one update;
   a path published twice in a window keeps the last value. Numbers use
   the shortest round-trip representation.
2. **Ring buffer** while the stream is down (`sk.buffer_msgs` /
   `sk.buffer_kb`, default 128 messages / 32 KiB): oldest messages are
   dropped first and counted (`ws.dropped`). Windows keep closing while
   offline, so a path's history survives, not just its latest value.
3. **Drain** after (re)connect at `sk.drain_per_s` (default 20/s) so the
   server is not swamped by a backlog; new values queue behind the backlog
   so ordering per path is preserved.

The WebSocket task (`espos_skws`) runs when `sk.ws_enabled`, WiFi is up, a
server is selected and the token state allows streaming (approved, or the
server has security off). It connects to
`ws://<host>:<port>/signalk/v1/stream?subscribe=none` with
`Authorization: Bearer <token>`, consumes the hello, then sends deltas as
text frames. A `401` on connect calls `espos_sk_report_unauthorized()` (the
token machine re-verifies / re-requests); any other failure backs off with
the shared WiFi backoff curve (`ws.next_retry_s`). Config changes to the
stream keys are picked up live; `ws_enabled=false` closes the socket and
keeps buffering.

**Meta reconciliation.** `espos_sk_declare_meta(path, meta_json,
period_ms)` records metadata for a NON-standard path (spec paths belong to
the server). On every (re)connect the task `GET`s
`/signalk/v1/api/vessels/self/<path>/meta`; if the server has none it
`PUT`s ours, otherwise the server's copy — possibly edited by the user —
wins. `period_ms > 0` adds `timeout` (2.5× the period, in seconds), the one
field the device really owns. `ws.meta.declared/reconciled` show progress.

**Device health.** Every `sk.health_s` (default 10 s, 0 = off) the task
publishes `espos.<hostname>.{uptime,freeHeap,minFreeHeap,internalFree,largestBlock,rssi,
wifiReconnects,skReconnects,resetReason}` with declared meta, so a
dashboard sees the device without any app code.

Wire facts that cost time (signalk-server 2.31): client text frames must
be sent with the FIN bit (`WS_TRANSPORT_OPCODES_TEXT |
WS_TRANSPORT_OPCODES_FIN`) or the server closes the socket after the
first frame; `subscribe=none` still delivers the hello; meta `GET` is
`404` when unset and `PUT` takes `{"value": {…}}`.

## Inbound (M7)

```c
int h = espos_sk_subscribe("navigation.*", 1000, on_update, NULL);   /* family */
espos_sk_subscribe("environment.mode", 0, on_update, NULL);           /* exact */
espos_sk_unsubscribe(h);
espos_sk_put("navigation.anchor.maxRadius", "30", on_put_done, NULL);
espos_sk_send_raw("{\"context\":\"vessels.self\",\"updates\":[…]}");
```

* **Subscriptions** are exact paths or families (`prefix.*`, `prefix*`,
  `*`). The stream is opened with `subscribe=none&sendMeta=all`; after the
  hello (and after every reconnect) one `{"context":"vessels.self",
  "subscribe":[{"path","period","format":"delta","policy":"instant",
  "minPeriod"}]}` frame carries every subscription; new ones while
  connected go out incrementally, `espos_sk_unsubscribe` sends
  `unsubscribe` when nothing else covers the pattern. Up to
  `ESPOS_SK_MAX_SUBS` (48).
* **Delivery**: `sk_parse.c` (pure C, cJSON) turns each frame into items —
  `path`, `value_json` (verbatim JSON text: numbers, strings, objects,
  `null`), `timestamp`, `$source`/`source.label`, `context` — plus meta
  items (`meta_json` set, value NULL) when the server sends `meta`. The
  callback runs on the stream task; copy what you need and return (a
  display marshals to its UI thread — never block, never call an
  `espos_sk_*` function that could wait on the stream). Frames are
  reassembled up to `CONFIG_ESPOS_SK_RX_FRAME_MAX` (16 KiB); larger ones
  are dropped with a log line.
* **PUT**: `{"context":"vessels.self","requestId":<uuid4>,"put":{"path",
  "value"}}`; the response (`state` COMPLETED/FAILED, `statusCode`,
  `message`) is matched by requestId and handed to the callback; no answer
  in 10 s → `"TIMEOUT"`. Up to 8 in flight; `ESP_ERR_INVALID_STATE` when
  the stream is down (nothing is queued across reconnects — a control
  action must not fire minutes later). A real server without a handler
  answers `COMPLETED` with `statusCode 405 "PUT not supported for …"`.
* **Raw frames** (`espos_sk_send_raw`) go out ahead of buffered deltas —
  e.g. an inbound `notifications.*` delta with `state:"normal"` to
  acknowledge an alarm.
* Status: `ws.in {subs, frames, received}`, `ws.put {pending, ok,
  failed}`; REST `POST /api/v1/sk/put {"path","value"}` (202; last
  answer under `GET /api/v1/sk/put`) for scripts.
* The example app subscribes to `app.watch_path` and logs each update.

Verified 2026-08-18 against signalk-server 2.31 on the ESP32-P4: 586
updates in ~40 s of `navigation.*` from N2K sources, satellitesInView
objects of several KiB reassembled, PUT round trip (405 from a server
without handlers).

## HTTP requests to the server

`espos_sk_http.h` is the one way an application talks HTTP to the selected
server. Four hand-rolled copies of "GET a SignalK REST node" in one firmware
had two of them rebooting the device; this is the version that does not.

```c
#include "espos_sk_http.h"

espos_sk_http_resp_t r;
if (espos_sk_http_get("/signalk/v1/applicationData/global/my-app/1/layout.json", NULL, &r) == ESP_OK
    && r.status == 200 && !r.truncated) {
    apply_layout(r.body, r.len);              /* NUL-terminated, malloc'ed */
}
espos_sk_http_resp_free(&r);

char *value = NULL;                          /* GET …/vessels/self/navigation/position → "value" member */
if (espos_sk_get_value("navigation.position", &value) == ESP_OK) { /* {"latitude":…,"longitude":…} */ }
free(value);
char *meta = NULL;                           /* GET …/navigation/speedOverGround/meta */
if (espos_sk_get_meta("navigation.speedOverGround", &meta) == ESP_OK) { /* {"units":"m/s",…} */ }
free(meta);

espos_sk_http_opts_t o = { .timeout_ms = 3000, .max_body = 512 };
espos_sk_http_post("/plugins/my-plugin/api/thing", "{\"on\":true}", &o, &r);   /* PUT, DELETE likewise */
espos_sk_http_resp_free(&r);

char url[ESPOS_SK_URL_MAX];
espos_sk_url("/signalk/v1/api", url, sizeof(url));       /* http(s)://host:port/signalk/v1/api */
espos_sk_ws_url("/signalk/v1/stream", url, sizeof(url)); /* ws(s)://… */
```

* **Reply**: `status` (0 when nothing arrived), `body` (malloc'ed,
  NUL-terminated, `""` for an empty reply), `len`, `truncated`. The call
  returns `ESP_OK` whenever a reply arrived — a 404 or 500 is a successful
  call; check `status`. `ESP_ERR_INVALID_STATE` means no server is selected,
  `ESP_ERR_TIMEOUT` that no connection slot came free, anything else is the
  transport error `esp_http_client` reported. Always
  `espos_sk_http_resp_free()`.
* **Options** (`NULL` or zeroed = defaults): 6 s timeout, 16 KiB body cap,
  `Authorization: Bearer` from the current token, 401/403 reported,
  `Accept: application/json`. `no_auth` drops the header, `no_report_unauthorized`
  keeps a 401 from touching the token machine, `accept` overrides the header
  (`""` = none).
* **Body cap**: the body is collected in `HTTP_EVENT_ON_DATA` and stops at
  `max_body`; beyond it `truncated` is set and the rest is drained and
  discarded. Never parse a truncated body — treat it as "the reply was too
  big" (`espos_sk_get_value/meta` return `ESP_ERR_INVALID_SIZE`). The buffer
  grows with the reply, so the cap costs nothing for small documents.
* **Token**: snapshotted per call from `espos_sk_get_token()`, sent as a
  header — never in a query string, where it would end up in every proxy and
  server log. No token, no header, which is what a server running without
  security expects. A 401/403 while a token was sent calls
  `espos_sk_report_unauthorized()`: the token machine re-verifies and, if the
  server really has dropped the device, requests access again.
* **Scheme**: from the selected server's `tls` flag, `http`/`ws` or
  `https`/`wss`, with the same certificate rules as the delta stream (below).
  `espos_sk_url()` / `espos_sk_ws_url()` build the URL for code that opens its
  own connection (the BLE gateway's control socket does).
* **Concurrency**: `CONFIG_ESPOS_SK_HTTP_MAX_CONCURRENT` (default 2, range
  1–8) bounds requests in flight — the token machine's and the meta
  reconciliation's own calls included. Each open request is a socket plus,
  over TLS, ~20 KB of RAM; a display fetching one value per widget on a layout
  change would otherwise open dozens at once. A caller over the limit waits
  up to its own `timeout_ms` for a slot.
* **Threading**: blocking, on the caller's task, for up to `timeout_ms`
  waiting for a slot plus `timeout_ms` on the wire, with ~2 KiB of its stack.
  Call from an application task. Never from the SK stream task (the
  `espos_sk_subscribe`/`espos_sk_put` callbacks), an `ESPOS_EVENT` handler, a
  Bluetooth stack callback or an HTTP URI handler.

### The two crash patterns it avoids

Both were reproduced on the ESP32-P4 against signalk-server, both are inside
`esp_http_client`, and both are why the helper insists on one particular
shape — a **fresh client per call** and **`esp_http_client_perform()` only**:

1. `esp_http_client_open()` → `fetch_headers()` → `read()` leaves the client's
   `cache_data_in_fetch_hdr` flag set. When the body arrives in the same TCP
   segment as the headers — which is every small SignalK reply — the next
   step hits `assert(orig_raw_data == raw_data)` in `http_on_body` and the
   device reboots. Only `perform()` clears the flag.
2. Reusing one handle across `perform()` calls (`set_url()` per path, one
   connection for a batch) desyncs the same two pointers and trips the same
   assert mid-batch (seen on `navigation.anchor.*` paths).

`perform()` with a new handle per request enters neither path; the body is
delivered through the event handler, which is also where the size cap lives.
`sk_http.c` has used this shape for the token legs since M3; the meta
reconciliation and the public API now share that single implementation.

## TLS (https / wss)

Off by default and inert unless the firmware was built with
`CONFIG_ESPOS_SK_TLS`, which compiles the TLS transports. With it, the
`sk.tls` setting switches the device to `https://` for the access-request
calls, every `espos_sk_http_*` request and the BLE gateway's POSTs, and to
`wss://` for the delta stream and the gateway's control socket; `sk.tls` is
`restart_required`, because the stream transport is built once when the
stream task connects.

What it is for: keeping the SignalK access token off the wire. The OTA path
— the one an attacker would actually want — is protected by image signatures
rather than by transport security (docs/ota.md), which is why plain `http`
remains a reasonable default on a boat LAN. On a shared marina network, or
anywhere the server is reachable from outside the boat, that reasoning stops
applying.

The scheme is a field on the chosen server (`espos_sk_server_t::tls`), not a
compile-time branch at each call site, so the day discovery learns to
advertise a scheme it becomes a value rather than a rewrite. Today nothing
sets it but the configuration: SignalK's mDNS advertisement does not say
whether the server speaks TLS.

**Limits, on purpose.** The server certificate is verified against the
bundled Mozilla roots. There is nowhere to pin a private CA yet, so a
self-signed certificate — what a boat server most often has — is refused
rather than accepted quietly; an "accept anything" switch would make the
setting a decoration.

The flash cost is near zero: a default espOS build already links mbedTLS and
the certificate bundle for signed OTA, and the image measured the same size
with and without `CONFIG_ESPOS_SK_TLS` on esp32c6. Budget ~20 KB of RAM for
the open connection.

## API

* `GET /api/v1/sk/status` — token/server/discovery status plus the `ws`
  stream object (see rest-api.md).
* `POST /api/v1/sk/put {"path","value"}` / `GET /api/v1/sk/put` — PUT over
  the stream and the last answer.
* `POST /api/v1/sk/publish {"path","value"[,"meta","period_ms"]}` — publish
  over HTTP.
* SSE `sk_ws` — the `ws` object on every stream change.
* `GET /api/v1/sk/servers` — discovered servers, `selected` flag.
* `POST /api/v1/sk/discover` — run a discovery pass now.
* `POST /api/v1/sk/request` — request again (from denied/error/open).
* `POST /api/v1/sk/token {"token"}` — manual token.
* `POST /api/v1/sk/forget` — drop the token and start over. A request that
  is still pending is kept and polled on (the server holds it anyway and
  refuses duplicates).
* SSE `sk`, `sk_servers`.

## Testing

* `test/host/espos_sk_test`: 32 Unity cases — token state machine, store,
  delta batching / ring / drain, frame parser and path patterns.
* `test/host/espos_httpd_test` `SkTests`: the real HTTP client and
  WebSocket against a Python mock of the signalk-server security API and
  stream endpoint (approve, deny, revoke, forget, security off, manual
  token, manual host, deltas + meta reconciliation, offline buffering with
  ordered drain; the HTTP helper through a harness probe: 200 with body and
  Bearer header, 404 as a reply, oversize body → `truncated`, value/meta/URL
  lookups, PUT through the same core, 401 → token machine re-requests);
  `SkInboundTests`: subscribe frames, value/meta delivery,
  exact vs family dispatch, PUT round trip / failure / timeout, raw frames,
  unsubscribe + resubscribe after reconnect, 9 KiB frame reassembly.
* Against a real signalk-server on the host: `node bin/signalk-server -c
  <fresh config dir>` from a checkout, `POST /skServer/enableSecurity
  {"userId","password","type":"admin"}`, restart, then approve with
  `PUT /skServer/security/access/requests/<clientId>/approved` using the
  admin cookie from `POST /signalk/v1/auth/login`; revoke with
  `DELETE /skServer/security/devices/<clientId>`.

## Notifications

`espos_sk_notify(key, state, message)` raises or clears a SignalK notification
under `notifications.espos.<label>.<key>`:

```c
espos_sk_notify("wakeService", ESPOS_SK_ALERT_WARN, "wake service unreachable");
espos_sk_notify("wakeService", ESPOS_SK_ALERT_NORMAL, "");   /* cleared */
```

For conditions the device knows about and an operator would want to see: memory
pressure, an overheating chip, a service the firmware depends on having gone
away. Without them these surface as a device that has quietly stopped doing its
job, which looks identical to a hardware fault and is the expensive kind of
problem to diagnose.

* **Level-triggered and idempotent.** Re-raising the same state and message
  sends nothing, so a caller may poll and re-raise on every tick. The first
  raise after boot always goes out, even if it is `NORMAL`, because the server
  may still hold an alert from before a restart.
* `key` is a short stable identifier (`lowMemory`, `wakeService`) -- it becomes
  part of the path, and the path is what a rule or dashboard keys on. The
  `message` is the human half and may change freely.
* `method` is `["visual"]` for warn/alarm and `[]` on clear. What to do about
  it is the server's decision, not the device's.
* Up to `CONFIG_ESPOS_HEALTH_MAX_CONDITIONS` distinct keys (default 8, range
  1-32): the notification is a sink on `espos_health`, so its condition table
  is the cap and one key too many gets `ESP_ERR_NO_MEM`. Deltas are buffered
  like any other while offline. Oversized keys or messages are rejected with
  `ESP_ERR_INVALID_SIZE` rather than truncated -- a clipped key would never
  match on the next call and would leak a slot.

espOS raises `lowMemory` itself, from `espos_health`'s watchdog policy rather
than the SignalK tick, so it exists without SignalK; thresholds and the
restart rule are in [health.md](health.md). Internal RAM is checked separately
because it is the scarce pool on targets with PSRAM -- tens of megabytes free
overall can hide an internal-RAM exhaustion that will take the radio down.

