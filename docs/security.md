# Security notes

What protects a device running espOS, what does not, and what to expect on
the wire. The short version: the REST API and web UI are guarded by a shared
API key once one is set, firmware images are signature-checked whatever their
source, and everything travels over plain http on the boat LAN.

## Threat model

An espOS device sits on a boat's own network next to the SignalK server,
the chartplotter and everyone's phone, and occasionally on a marina's shared
WiFi. Its REST API can change any setting, reboot the device, factory-reset
it, drop and re-request the SignalK token, and — through an application's own
endpoints — do whatever the firmware exposes. The people to keep out are:

* **the casual and the accidental** — a guest's phone that found
  `espos-1a2b.local`, a script pointed at the wrong host, a browser tab left
  open on the marina WiFi;
* **a hostile web page** in the operator's own browser, trying to drive the
  device through that browser (CSRF);
* **the curious neighbour** on a shared network who can send requests but is
  not capturing traffic.

Not in the model: someone **sniffing the LAN** (the key crosses it in clear,
see Transport), someone with **the USB port** (no Secure Boot, no flash
encryption in a development build), and the SignalK server itself, which the
device trusts by design.

## REST authentication

Every endpoint is *protected* unless it is one of the few *public* ones
([rest-api.md](rest-api.md) marks each). Protection is decided by one
setting, **`httpd.api_key`**:

| `httpd.api_key` | Effect |
|---|---|
| empty (the default) | the API is **open** to anyone on the network, as before — a device that predates authentication keeps working after an update, and the first setup needs no key |
| set (8–64 characters) | every protected request needs a credential, else `401` |

Two credentials, one central check, before any handler runs:

* **`Authorization: Bearer <key>`** — for machine clients: the
  signalk-hmi-designer, a fleet plugin, `curl`. Stateless; nothing is kept on
  the device.
* **A session cookie** — for browsers. `POST /api/v1/auth/login {"key": …}`
  answers with `espos_sid`, `HttpOnly; SameSite=Strict; Path=/`, valid for
  `httpd.session_ttl_s` (a day by default). `fetch()` and `EventSource` send
  it by themselves; the web UI shows its login page when the device asks.
  Sessions are a small RAM table (`CONFIG_ESPOS_HTTPD_MAX_SESSIONS`, 4): one
  login too many evicts the one idle longest, a reboot forgets them all, and
  changing the key drops every session at once — whoever knew the old key
  logs in again with the new one.

A cookie is sent by the browser with whatever a page asks for, so a
cookie-authenticated **state-changing** request (`PUT`, `POST`, `DELETE`)
must also come from the device's own origin: `Origin` (or `Referer`) host
must equal `Host`, else `403 forbidden`. This sits on top of the existing
rule that state-changing requests carry `Content-Type: application/json`,
which a browser cannot send cross-origin without a CORS preflight the device
never grants. Bearer requests skip the origin check: the header is not
something a browser adds on its own.

Guessing is slowed rather than blocked: after **five wrong keys within 60 s**
every key check — login and Bearer alike, the right key included — answers
`429 too_many_attempts` with `Retry-After` for **30 s**. The counter is
global (a device cannot tell clients apart cheaply); live cookies are not
key checks and keep working. Key and session-id comparisons run in constant
time over the maximum length.

Applications get this for free: an endpoint registered through
`espos_httpd_register()` is protected, an endpoint that must stay reachable
registers with `espos_httpd_register_ex(uri, ESPOS_HTTPD_PUBLIC)`, and a
public handler can still ask `espos_httpd_request_authenticated()`. The
public set is the UI bundle, `GET /api/v1/system/ping` (liveness: app,
version, whether a key is wanted), the captive-portal probe URLs, and
`/api/v1/auth/*` itself.

### The setup portal is exempt — and is the way back in

A request that arrives **on the device's own access point** (the soft-AP
that comes up while it has no WiFi to join, or when the station is
disabled) bypasses authentication: whoever stands next to the device and
joined `espOS-xxxx` is its operator. The check compares the local socket
address with the AP interface's IP, so it is not a header a remote client
could forge, and it never applies to a request that came in over the
station link.

This is also the **lockout recovery**: a lost key is replaced by a factory
reset (the button or `POST /api/v1/system/factory-reset`; the settings,
WiFi and SignalK token go with it), joining the portal at
`http://192.168.4.1`, and entering a new key on the Config page — no key
needed on that network. There is no other back door: no default key, no
reset URL on the station side.

### For products: `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED`

The open default is right for a device someone flashes at home. A product
that must never ship open builds with `CONFIG_ESPOS_HTTPD_AUTH_REQUIRED=y`:
while `httpd.api_key` is empty, protected endpoints answer
`403 auth_unconfigured` instead of serving — except from the portal
network, where the first key gets set. The web UI explains exactly that when
it sees the 403.

### What it does and does not stop

Does: the guest's phone, the misdirected script, the tab on the marina WiFi,
the hostile page in the operator's browser, and idle guessing.
Does not: anyone who can **capture traffic** between a client and the device
— over plain http the key crosses the network in clear on every Bearer
request and at every login, exactly the way the SignalK access token does
between the device and the server. On a boat's own network that is the
accepted trade (the same network carries unauthenticated NMEA); on a shared
marina network, treat the key as exposed and change it back home.

## Transport

Traffic to the SignalK server is plain `http`/`ws` unless the firmware is
talking to the server over TLS (`sk.scheme`, `auto` by default)
([signalk.md](signalk.md)). The consequence to be clear about: the access
token travels in an `Authorization` header over an unencrypted connection,
so anyone who can capture traffic on the boat LAN can replay it against the
server with whatever permissions the token was granted.

The device's own web server is http-only, deliberately. A TLS server costs
RAM the smaller targets do not have to spare, and a self-signed certificate
on a boat LAN gives the browser nothing to verify against. The Kconfig
symbol `CONFIG_ESPOS_HTTPD_TLS` is **reserved** (off, no effect) so that a
future implementation has a stable name; until then the API key protects
against use, not against eavesdropping. Firmware updates do not depend on
transport security either way: images are signature-verified by the running
app, so a plain-http image source cannot be substituted (see below).

## Secrets at rest

The API key, WiFi passwords and the SignalK token live in NVS, marked
`secret` in their descriptors: the API never returns them (they read back as
`"********"`), and the web UI shows only whether they are set. For
production builds enable flash encryption; IDF then defaults
`CONFIG_NVS_ENCRYPTION=y` and encrypts the `nvs` partition transparently
using keys in the `nvs_keys` partition (present in every bundled
`partitions/*.csv`, flagged `encrypted`), or the HMAC peripheral on chips
that have one.

The release overlay is `sdkconfig.d/release.defaults`, selected with
`espos_project_prologue(... PROFILE release)` or `-DESPOS_PROFILE=release`:

```
CONFIG_SECURE_FLASH_ENC_ENABLED=y
CONFIG_SECURE_FLASH_ENCRYPTION_MODE_RELEASE=y
CONFIG_NVS_ENCRYPTION=y
```

Development boards stay unencrypted so `idf.py flash` keeps working
without burning eFuses. Note: `nvs_flash` on the linux host target does not
support encryption (host tests always run unencrypted).

## Request handling

* Authentication is checked before a handler runs, for every registered
  endpoint; a refused request never reaches the handler's body parsing.
* Request bodies are capped (`CONFIG_ESPOS_HTTPD_MAX_BODY`, default 16 KiB;
  `413` beyond that).
* All JSON input is validated against the descriptor before any write.
* Config values are validated on *read* too, so a hostile or stale NVS
  content cannot push out-of-range values into the application.
* Static file paths refuse `..` and `//`; anything under `/api/` that is
  not registered is a JSON `404`, never a file.

## Firmware updates

Every app image is signed (RSA-3072, `SECURE_SIGNED_APPS_NO_SECURE_BOOT`)
and the running firmware verifies the signature of any update it writes
against its compiled-in public key, so a device on the network only takes
firmware from whoever holds `secure_boot_signing_key.pem` — regardless of
whether the image came over `http://` or `https://`, and regardless of who
asked for the update (`POST /api/v1/ota` is protected like everything
else). This does **not** stop someone with the USB port (no hardware Secure
Boot, no flash encryption); those remain release-overlay options. Rollback
protection is the bootloader's `APP_ROLLBACK_ENABLE` plus the
confirm-on-network policy in `espos_ota` ([ota.md](ota.md)). Keep the
signing key out of the repository: it is git-ignored, and a missing key
yields a *development* key with a loud CMake warning.
