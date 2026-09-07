# two_phase_boot — **Advanced**

`espos_init()` and `espos_start_network()` instead of one `espos_start()`,
with the application's own work between them; and the pattern for an espOS
call that blocks — a worker task fed through a queue. Replaces SensESP's
`freertos_tasks`. No wiring; builds for every target (esp32c6 and esp32p4
verified).

## What it does

1. **Phase one** — `espos_init()`: the log ring and the config store.
   Settings are readable from here on; nothing talks to the network yet.
2. **The local thing** — a stand-in for a display or a sensor bus that must
   exist before the network: one log line and a worker task with a queue.
   This is where a panel shows the portal SSID before there is a portal, or
   a peripheral settles before the radio draws its start-up current.
3. **Phase two** — `espos_start_network()`: httpd → wifi → sk → ota. The
   one-call equivalent is `espos_start()` with `before_network` set (shown
   in a comment in `main.c`); two calls are for when the work in between is
   not a single function — it spawns tasks, or needs `app_main`'s stack.
4. Once a second a reading (`environment.outside.pressure`, in Pa) goes into
   the queue; the worker publishes it.
5. On every `ESPOS_EVENT_SK_STREAM_CONNECTED` the handler posts one job and
   the worker runs `espos_sk_get_value("navigation.position", …)`, logging
   the result: the current position from the server's REST tree, the way a
   display backfills a value the stream will not repeat until it changes.

In Signal K: `environment.outside.pressure` from the device's source. On the
monitor after each connect: `navigation.position = {"latitude":…}`, or
`navigation.position: ESP_ERR_NOT_FOUND` on a server with no position.

## Why the queue

Every callback in espOS runs on a task that is not yours (docs/concepts.md,
"Threading contracts"). `ESPOS_EVENT` handlers run on the default event
loop — the task the WiFi driver's own events queue behind — and
`espos_sk_get_value()` blocks for up to twice its timeout; one called from
the other stalls WiFi. So the handler posts a word-sized job and returns,
and the worker, a task the application owns, does the waiting. The same
shape serves a display's UI thread or a bus receiver: one owner per
resource, hand-offs by value.

Publishing has no such rule — `espos_sk_publish_*` is thread-safe and never
blocks — but the engine behind it is created by phase two, so the readings
start after it; before, the call returns `ESP_ERR_INVALID_STATE`.

## Build

```sh
. $IDF_PATH/export.sh
cd components/espos_core/examples/two_phase_boot
idf.py set-target esp32c6 && idf.py build flash monitor
# esp32p4: idf.py set-target esp32p4 — WiFi is the C6 co-processor (main/idf_component.yml)
```
