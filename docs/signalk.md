# SignalK (`espos_sk`) — discovery and access token

M3 scope: find the server, get and keep a token. Delta output and meta
reconciliation are M4 and build on `espos_sk_get_token()` /
`espos_sk_get_server()`.

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

## API

* `GET /api/v1/sk/status` — token/server/discovery status (see api.md).
* `GET /api/v1/sk/servers` — discovered servers, `selected` flag.
* `POST /api/v1/sk/discover` — run a discovery pass now.
* `POST /api/v1/sk/request` — request again (from denied/error/open).
* `POST /api/v1/sk/token {"token"}` — manual token.
* `POST /api/v1/sk/forget` — drop the token and start over. A request that
  is still pending is kept and polled on (the server holds it anyway and
  refuses duplicates).
* SSE `sk`, `sk_servers`.

## Testing

* `test/host/espos_sk_test`: 17 Unity cases over the state machine.
* `test/host/espos_httpd_test` `SkTests`: the real HTTP client against a
  Python mock of the signalk-server security API (approve, deny, revoke,
  forget, security off, manual token, manual host).
* Against a real signalk-server on the host: `node bin/signalk-server -c
  <fresh config dir>` from a checkout, `POST /skServer/enableSecurity
  {"userId","password","type":"admin"}`, restart, then approve with
  `PUT /skServer/security/access/requests/<clientId>/approved` using the
  admin cookie from `POST /signalk/v1/auth/login`; revoke with
  `DELETE /skServer/security/devices/<clientId>`.
