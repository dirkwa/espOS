# json_and_meta — **Advanced**

The three publish calls a plain number does not cover, framed as a windlass
controller: `espos_sk_publish_json()` for an object value (where the anchor
went down), `espos_sk_declare_meta()` for a path only this device knows (how
much rode is out), `espos_sk_notify()` for a condition of the device itself
(the windlass motor overloading). No hardware; the values are simulated.

Replaces SensESP's `raw_json` and the `metadata` example (SensESP issue #501).

## What appears in Signal K

| Path | Value | Meta |
|---|---|---|
| `navigation.anchor.position` | `{"latitude": 54.3233, "longitude": 10.1394}` | the server's: a spec path |
| `navigation.anchor.rodeDeployed` | 0..40, up and down, every 5 s | declared by the device: `units` m, `description`, `timeout` 12.5 s |
| `notifications.espos.<hostname>.windlassOverload` | `warn` while the rode is between 20 and 30 m, `normal` otherwise | — |

All from source `espos.<hostname>`. The `ws.meta` counters in
`GET /api/v1/sk/status` show the declared meta being reconciled after each
connect.

**Run this against a spare server, or change `POSITION_PATH`.** It publishes
a constant anchor position; on a boat whose anchor-alarm plugin uses that
path, the plugin's value and this one would compete.

## Who owns meta

Metadata belongs to whoever defines the path — the comment block in
`main/main.c` is the rule in full. In short: never declare meta for a spec
path (the server has it and the user may have edited it); for a custom path
declare the *whole* object, because what is PUT replaces the meta rather than
merging into it; `period_ms` adds the one field only the device can know, the
`timeout` after which a dashboard should treat the value as stale. `espos_sk`
reconciles on every (re)connect and only PUTs when the server has nothing, so
an edit made on the server always wins.

## Notifications

`espos_sk_notify(key, state, message)` is level-triggered and idempotent:
call it on every tick with the current state and only changes go out; the
first call after boot always goes out, so an alert left over from before a
reboot is retired. The key is an identifier, not a sentence — it becomes the
path a rule or a dashboard keys on. It forwards to `espos_health_report()`
(docs/health.md), which is the same call without a dependency on the SignalK
stack; use that one in code that must also build without SignalK.

## Build and flash

```sh
. $IDF_PATH/export.sh                   # ESP-IDF v6.0.2, see .idf-version
cd components/espos_sk/examples/json_and_meta
idf.py set-target esp32c6               # or esp32p4
idf.py build flash monitor              # shared or small host: ../../../../scripts/build.sh build
```

First boot: join the `espOS-xxxx` access point, open http://192.168.4.1 and
pick your WiFi; then approve the device in signalk-server (Security → Access
Requests). The Data Browser shows all three paths within a few seconds.
