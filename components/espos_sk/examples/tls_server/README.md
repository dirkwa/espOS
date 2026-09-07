# tls_server — **Advanced**

The SignalK connection over https and wss instead of http and ws: the
access-request calls, every `espos_sk_http_*` request and the delta stream.
The application code does not change — the scheme is a property of the
selected server, not of any call — so this example is mostly configuration,
plus one diagnostic GET that tells you whether the certificate verified.

Replaces SensESP's `ssl_connection`.

## What works, and what does not (yet)

* **Works:** a server whose certificate chains to a root in the bundled
  Mozilla set — a public hostname behind a reverse proxy with a Let's Encrypt
  certificate, say. `sk.server_host` must then be that hostname, not an IP
  address: the certificate is checked against the name you connect to.
* **Fails today, by design:** signalk-server's own self-signed certificate
  (what its `ssl: true` setting generates) and any private CA. There is
  nowhere yet to pin one, and an "accept anything" switch would make the
  setting a decoration; trust-on-first-use / pinning comes in a later
  tranche. The symptom: the probe logs `GET https://… failed: …` right after
  esp-tls reports the handshake failure (mbedTLS -0x2700, certificate verify
  failed); `ws.last_error` in `GET /api/v1/sk/status` says the same.
* **Cost:** about 20 KB of RAM while the stream is open, and the Mozilla root
  bundle in flash — 64 KB in this example, nothing in a firmware that links it
  already. What it buys is the token off the wire: worth it on a shared marina network.

## Two settings and one Kconfig

* `CONFIG_ESPOS_SK_TLS=y` (this example's `sdkconfig.defaults`) compiles the
  https/wss transports. Without it `sk.tls` is inert and the device logs so.
* `sk.tls` — the switch, per install; `restart_required`, because the stream
  transport is built once when the stream task connects. This example seeds
  it to `true` on first boot when it has never been set (see `app_main()`);
  turn it off in the web UI and it stays off.
* `sk.server_host` / `sk.server_port` — a TLS server is reached by name.
  mDNS discovery says nothing about TLS and hands out addresses, so set the
  host explicitly (which also stops discovery):

```sh
curl -X PUT http://<hostname>.local/api/v1/config -H 'Content-Type: application/json' \
     -d '{"sk": {"server_host": "signalk.example.org", "server_port": 443, "tls": true}}'
curl -X POST http://<hostname>.local/api/v1/system/reboot -H 'Content-Type: application/json'
```

The scheme then follows the server: `espos_sk_get_server()` reports `tls`, `espos_sk_url()`
builds `https://…`, `espos_sk_ws_url()` `wss://…`, and the monitor logs the probe's result.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/tls_server
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; set the server as above; approve the device in signalk-server
(Security → Access Requests). A counter appears under `espos.<id>.heartbeat`.
