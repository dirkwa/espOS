# SignalK (`espos_sk`) — discovery, access token, delta stream, inbound

M3: find the server, get and keep a token. M4: stream published values as
deltas over a WebSocket, buffer them while offline, reconcile metadata,
publish device health. M7: subscribe to paths and families, receive values
and meta, send PUT requests and raw frames — what a display or controller
needs on top of a sensor.

## Discovery

`espos_sk` browses `_signalk-http._tcp` via mDNS every `sk.discover_s`
(default 60 s, immediately when WiFi comes up) and keeps up to 6 servers
with their TXT records (`self`, `roles`, `swname`, `swvers`). Entries that
drop out of one query survive two intervals (mDNS is lossy). The device
also advertises itself under its hostname. `GET /api/v1/sk/servers` lists
them; `sk_servers` SSE events fire after every pass.

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
publishes `espos.<hostname>.{uptime,freeHeap,minFreeHeap,rssi,
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

## API

* `GET /api/v1/sk/status` — token/server/discovery status plus the `ws`
  stream object (see api.md).
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
  ordered drain); `SkInboundTests`: subscribe frames, value/meta delivery,
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
* Up to `CONFIG_ESPOS_SK_MAX_NOTIFY` distinct keys (default 8, range
  1-32); buffered like any other delta while offline. Oversized keys or
  messages are rejected with `ESP_ERR_INVALID_SIZE` rather than truncated --
  a clipped key would never match on the next call and would leak a slot.

espOS raises one itself: **`lowMemory`**, from the health tick, when internal
RAM drops below 20 KB or the heap below 40 KB. Internal RAM is checked
separately because it is the scarce pool on targets with PSRAM -- tens of
megabytes free overall can hide an internal-RAM exhaustion that will take the
radio down.

