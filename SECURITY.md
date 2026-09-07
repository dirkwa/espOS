# Security

## Reporting a vulnerability

Report privately through GitHub's security advisories:
<https://github.com/espos/espos/security/advisories/new> ("Report a
vulnerability" on the Security tab). Do not open a public issue or a
Discussion for anything that could be exploited on a device that is already
out there.

You will get an acknowledgement within seven days. A confirmed issue is fixed
in the next release and the advisory is published with credit unless you ask
otherwise. Consumers learn about it through the release notes and
`CHANGELOG.md`; there is no separate announcement channel.

## Supported versions

espOS is consumed as a git submodule and versioned pre-1.0, where a minor
bump does the work a major will do later ([docs/releasing.md](docs/releasing.md)).
Fixes go to the latest minor release line only:

| Version | Supported |
|---|---|
| latest minor (currently 0.7.x) | yes |
| earlier | no — bump the submodule |

## What is in scope

espOS runs on a boat's own network next to unauthenticated NMEA traffic, and
its threat model is written down in [docs/security.md](docs/security.md).
Read that first; it decides what is a vulnerability and what is a documented
trade-off.

In scope — please report:

* a secret (WiFi password, SignalK token, signing material) reaching the
  REST API, the log ring, the SSE stream or a core dump; the API is designed
  never to return secrets;
* a way to make the device accept an OTA image that is not signed by the
  holder of the signing key, or to bypass rollback;
* memory-safety problems reachable from the network: request parsing, JSON
  handling, the WebSocket and SignalK inbound paths, BLE, the NMEA 2000 and
  candump-over-TCP paths;
* a regression of the CORS/`Content-Type` gate that lets a web page the user
  has open reboot, reset or reconfigure a device cross-origin;
* once the authentication tranche has shipped: any bypass of it.

Out of scope, by design, for now:

* **The REST API is unauthenticated.** Anyone with network access to the
  device can read every non-secret setting, change any setting, reboot the
  device or factory-reset it. This is deliberate until the authentication
  tranche lands (the candidates are a local admin password or reuse of the
  SignalK token), the same trade SensESP's configuration UI makes, and it is
  the reason the device belongs on the boat's own network and not on a
  shared marina one. Reports that amount to "the API has no login" are
  known; reports that show a way *around* the controls that do exist
  (secrets never returned, the cross-origin gate) are welcome.
* Traffic to the SignalK server is plain `http`/`ws` unless the firmware is
  built with `CONFIG_ESPOS_SK_TLS` and `sk.tls` is on; the token travels in
  clear on the LAN. Documented, and a consumer's choice.
* Physical access: the USB/JTAG port, flash read-out, eFuse state. Hardware
  Secure Boot and flash encryption are release-overlay options a consumer
  turns on; espOS does not enable them for development boards.
* The development signing key generated on first build: it is a placeholder
  with a loud warning, not a secret.
