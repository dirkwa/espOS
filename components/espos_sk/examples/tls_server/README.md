<!--
SPDX-FileCopyrightText: 2026 Dirk Wahrheit
SPDX-License-Identifier: Apache-2.0
-->
# tls_server — **Advanced**

The SignalK connection over https and wss instead of http and ws: the
access-request calls, every `espos_sk_http_*` request and the delta stream.
The application code does not change — the scheme is a property of the
selected server, not of any call — so this example is one diagnostic GET plus
the settings worth knowing about.

Replaces SensESP's `ssl_connection`.

## It now works against a real boat server

The whole point of the S1 trust store: **signalk-server's own self-signed
certificate**, which is what `ssl: true` generates and what almost every boat
actually has. It used to be refused, because the only thing the device could do
with a certificate was check it against the bundled Mozilla roots.

With `sk.tls_trust` at its default `tofu` the first connection that works pins
what the server presented, and every later one has to match — the way ssh does
it. A private CA is pinned as the CA plus the names on the leaf, so renewals by
that CA for those names go through untouched; a bare self-signed certificate is
pinned as itself, and replacing it needs one deliberate press of **Trust the
new certificate** on the device's SignalK page (or `DELETE /api/v1/sk/tls`).

There is no accept-anything mode. `sk.tls_trust = bundle` is still there for a
server with a real certificate from a real CA.

## Nothing to configure for the common case

`sk.scheme` defaults to `auto`, and signalk-server advertises
`_signalk-https._tcp` instead of `_signalk-http._tcp` when its `ssl` setting is
on — so a discovered server tells the device which scheme it speaks and the
device follows. A manual host gets one unauthenticated probe of the plain port
instead, and a `30x` to an `https://` Location decides it (the port comes from
the Location too).

So: turn SSL on in signalk-server, and this example should connect over wss
with nothing set on the device at all. The monitor prints the probe:

```
I tls_server: server 192.168.1.10:443 over https/wss
I tls_server: GET https://192.168.1.10:443/signalk/v1/api -> HTTP 200, 412 bytes
I espos_sktls: pinned the server's certificate for signalk.local
```

A refusal says which way it differed rather than looking like an unreachable
server:

```
W tls_server: GET https://…: the server certificate changed -- see GET /api/v1/sk/tls
```

`GET /api/v1/sk/tls` then shows the pinned identity next to the presented one,
which is what makes it actionable: if the two fingerprints differ and you
replaced the certificate yourself, trust the new one; if you did not, find out
what is answering as your server first.

## The settings

* `CONFIG_ESPOS_SK_TLS` — **on by default now**; this example's
  `sdkconfig.defaults` no longer has to ask for it. Turning it off drops the
  transports and mbedTLS with them.
* `sk.scheme` — `auto` (default) / `http` / `https`. Not `restart_required`
  any more: the stream destroys and rebuilds its transport when the scheme
  changes, so a server that gains or loses SSL is followed live.
* `sk.tls_trust` — `tofu` (default) / `ca` / `bundle`.
* `sk.ca_pem` — the issuing CA for `ca` mode, or `PUT /api/v1/sk/tls/ca
  {"pem": "-----BEGIN CERTIFICATE-----…"}`. Pre-seed it on a fleet and the
  first connection is already verified with nothing to capture.
* `sk.server_host` / `sk.server_port` — only for a network without mDNS. An IP
  address is fine: outside `bundle` mode the certificate is matched by
  fingerprint and SAN set, not by the name you dialled.

```sh
# force https, and hold the server to your own CA
curl -X PUT http://<hostname>.local/api/v1/sk/tls/ca -H 'Content-Type: application/json' \
     -d "{\"pem\": \"$(sed -z 's/\n/\\n/g' boat-ca.crt)\"}"
# what is trusted, and what the server last showed
curl -s http://<hostname>.local/api/v1/sk/tls
# accept a certificate you replaced yourself
curl -X DELETE http://<hostname>.local/api/v1/sk/tls
```

## Cost

Measured on esp32c6 with the reference app: **no flash difference at all** when
`espos_ota` is in the build, because an https image source already links
mbedTLS and the certificate bundle. A build without it pays about **64 KB**,
nearly all of it the Mozilla bundle. RAM: ~20 KB while a connection is open,
and the device refuses a handshake below
`CONFIG_ESPOS_SK_TLS_MIN_FREE_BLOCK_KB` (24 KB) of contiguous internal RAM
rather than failing halfway through one.

What it buys is the access token off the wire: worth it on a shared marina
network, or anywhere the server is reachable from outside the boat.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/tls_server
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; approve the device in signalk-server (Security → Access
Requests). A counter appears under `espos.<id>.heartbeat`.
