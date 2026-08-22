# Device health

`espos_health` is where a component says what is wrong with the device, and
something else decides what to do about it. It is deliberately tiny: a table
of conditions, a list of sinks, no dependencies beyond FreeRTOS and the log.

```c
#include "espos_health.h"

/* Level-triggered: call it as often as you like. */
if (!bus_answering) {
    espos_health_report("n2kBus", ESPOS_HEALTH_WARN, "no frames for 30 s");
} else {
    espos_health_report("n2kBus", ESPOS_HEALTH_NORMAL, "");
}
```

A condition is a **level, not an event**. Reporting the same state and message
twice calls the sinks once, so the natural shape is to re-report on every poll
of whatever you are watching rather than to track edges yourself.
`ESPOS_HEALTH_NORMAL` clears the condition and is delivered like any other
change — it is what retires an alert raised before the last reboot.

The key (`"n2kBus"`, `"wakeService"`, `"lowMemory"`) is an identifier, not a
sentence: it ends up in a SignalK path and is what a rule or a dashboard keys
on. The message is the human-readable half and may change without
re-notifying. Both are rejected, not truncated, when too long — a clipped key
would never match on the next call.

## Sinks

Anything may consume conditions:

```c
static void led_sink(const char *key, espos_health_state_t state,
                     const char *message, void *arg)
{
    gpio_set_level(STATUS_LED, espos_health_worst() != ESPOS_HEALTH_NORMAL);
}

espos_health_add_sink(led_sink, NULL);
```

Registering replays every condition recorded so far, so a sink that comes up
late still learns the current state instead of waiting for the next change.
Sinks run on the reporting task with no lock held: report from inside one if
you want, but do not block for long.

`espos_sk` registers a sink of its own when `ESPOS_SK_NOTIFICATIONS` is on,
publishing each condition to `notifications.espos.<label>.<key>` with
`state`/`message` and a `visual` method. `espos_sk_notify()` is still there and
still works — it forwards to `espos_health_report()`, and is the right call
only when you specifically want the report refused if SignalK is not running.

## Why it is not part of espos_sk

Reporting a fault is a core concern; SignalK is one sink for it. When the only
way to raise a condition was `espos_sk_notify()`, `espos_voice` — a Wyoming
satellite that has nothing to do with SignalK — had to depend on the whole
SignalK stack for its one notification, and a firmware built without SignalK
could not report anything at all. One optional component depending on another
for a core concern generalises badly: the N2K gateway wants to report a silent
bus, an application wants to report its sensor, and each such edge drags the
SignalK stack behind it.

## Sizing

`CONFIG_ESPOS_HEALTH_MAX_CONDITIONS` (default 8) is the number of distinct
keys; `CONFIG_ESPOS_HEALTH_MAX_SINKS` (default 4) the number of consumers.
Both are fixed tables — the set of conditions a firmware can raise is decided
at build time. Reporting a key beyond the limit returns `ESP_ERR_NO_MEM` and
logs; it never grows silently.
