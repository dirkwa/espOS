# Time

An ESP32 boots with no idea what time it is. The only clock it has is a
counter that starts at zero, which is enough to schedule work and useless for
saying *when* a measurement was taken.

That gap is not academic. A device buffers values while the server is
unreachable — an anchorage out of WiFi range, a router rebooting, a server
being restarted — and replays them minutes or hours later. A SignalK delta
without a `timestamp` is stamped by the server *when it arrives*, so an hour of
wind data recorded during an outage lands in the log as one burst at
reconnect, in the wrong place and in the wrong order. The graph shows a flat
line and then a spike; the data is there and it is a lie.

`espos_time` closes that gap, and `sk.timestamps` (on by default) is what
spends it: every delta carries the instant its values were measured, even when
the device only learned the time long afterwards.

## Sources, and who wins

The clock is learned from whichever source got there first, and a source never
overrides one that outranks it:

| Rank | Source | Where it comes from |
|---|---|---|
| 4 | `sntp` | An NTP server, once the network is up (`time.sntp`, `time.server0`, `time.from_dhcp`) |
| 3 | `manual` | `PUT /api/v1/time`, or `espos_time_set()` from an application with a GPS or an RTC chip |
| 2 | `sk` | `navigation.datetime` on the SignalK stream (`time.sk_fallback`) |
| 1 | `rtc` | An instant carried through a deep sleep in RTC memory |
| 0 | `none` | Nothing has said what time it is |

The ordering is the point. NTP is the most accurate thing available, so once it
has answered, nothing may walk the clock back — not an operator typing into a
form, and certainly not a coarse timestamp off the stream. A source may always
*refresh itself*, so SNTP re-syncing every hour is normal and not a downgrade.

`time.sk_fallback` matters more at sea than it looks. A boat's SignalK server
usually has GPS time whether or not the boat has an internet route, so a device
that can reach the server can learn the time even where NTP never answers —
port 123 blocked, no uplink at all. That is the common case in an anchorage,
not an edge case, which is why it is on by default. The subscription is dropped
as soon as a better source succeeds.

### Deep sleep

A device that naps between measurements stores the instant in RTC memory and
adopts it on waking, so it is not blind until the network comes back. That
value drifts — the RTC oscillator is not a timekeeping crystal — so after
`CONFIG_ESPOS_TIME_RTC_STALE_H` (24 by default) the device still *reports* the
time but stops calling itself synced. A consumer that needs a trustworthy
timestamp can tell the difference; a log line is happy with an approximation.

The record is only adopted after an actual deep-sleep wake. After a power-on,
a panic or an OTA reboot the RTC memory may hold an intact-looking record that
is arbitrarily old, and a device switched off for a month would otherwise come
up certain it was still last month.

## Reading the clock

```c
#include "espos_time.h"

if (espos_time_is_synced()) {
    char iso[ESPOS_TIME_ISO_MAX];
    espos_time_iso8601(iso, sizeof(iso));   /* "2026-09-07T10:12:13.456Z" */
}
int64_t ms = espos_time_now_ms();           /* unix ms, or 0 when unsynced */
```

`espos_time_now_ms()` returns **0** while unsynced rather than a
plausible-looking 1970, and `espos_time_iso8601()` writes `""`. A caller that
forgets to check therefore produces an obviously missing value instead of a
quietly wrong one, which is the difference between a gap in a graph and a
decade of bad data at the origin.

To be told when a source sets it:

```c
static void on_clock(espos_time_src_t src, void *arg) { /* redraw a display */ }
espos_time_subscribe(on_clock, NULL);
```

The callback runs on the task of whatever source set the clock — IDF's SNTP
task, the SignalK stream task, the HTTP server's — with no `espos_time` lock
held. Copy what you need and return. `ESPOS_EVENT_TIME_SYNCED` carries the same
news on the event bus for anything that would rather subscribe there.

## Timezones

**Everything espOS publishes is UTC.** SignalK is a UTC protocol and the data
path never looks at a timezone; there is no setting that changes what goes on
the wire.

Local time is a *display* concern — a panel telling the crew what o'clock it
is — and that is the only thing `time.tz` and `espos_time_parts()` are for:

```c
espos_time_set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
espos_time_parts_t p;
if (espos_time_parts(&p) == ESP_OK) {
    printf("%02u:%02u\n", p.hour, p.minute);
}
```

Note the POSIX sign convention: the offset is written **west-positive**, so
central Europe (UTC+1) is `-1`. `espos_time_parts_t` is a plain struct rather
than `struct tm` because the public headers carry no platform types.

## Delta timestamps

With `sk.timestamps` on and the clock set, every message the delta engine hands
out gains `updates[].timestamp`:

```json
{"context":"vessels.self","updates":[{"timestamp":"2026-09-07T10:12:13.456Z",
  "source":{"label":"espos-1a2b"},
  "values":[{"path":"environment.wind.speedApparent","value":3.6}]}]}
```

The engine records the monotonic instant each message was built and converts it
at send time with the clock's *current* offset — `wall_now - (mono_now -
batch_ms)`. That is what makes a late sync work: a device that boots with no
network, buffers an hour of measurements, and only then reaches an NTP server
still dates every buffered message to the second it was taken. The alternative
— stamping at build time — would leave exactly the messages that need a
timestamp most without one.

When the clock is unset the timestamp is simply omitted and the server stamps
on arrival, which is the behaviour espOS had before this existed. Nothing
breaks; the data is just less precise.

A message that was already sent and came back through
`espos_sk_delta_requeue()` (the send failed) keeps the stamp it had. Retrying
does not move the time of a measurement that did not move.

## Log lines

With `CONFIG_ESPOS_LOG_WALLCLOCK` (default on) and a clock, lines stored in the
ring for `/api/v1/logs` carry both times:

```
I (18452|2026-09-07T10:12:13.456Z) espos_sk: stream connected
```

The monotonic milliseconds stay exactly where they were — that is what you read
to see how long after boot something happened — and the UTC time joins them
inside the same parentheses, so the level letter stays at offset 0 and anything
colouring lines by severity keeps working. Console output is untouched; only
the stored line is stamped. Lines logged before the clock is set are unchanged,
so a boot log reads as it always did.

## REST

`GET /api/v1/time` and `PUT /api/v1/time` are in the [REST API](rest-api.md);
`/system/info` carries a `time` object with the same `synced`/`source`/`now`.

## Configuration

| Key | Default | What it does |
|---|---|---|
| `time.sntp` | `true` | Ask an NTP server once the network is up. Restart required. |
| `time.server0` | `pool.ntp.org` | NTP server. Restart required. |
| `time.server1` | `""` | Fallback NTP server. Restart required. |
| `time.from_dhcp` | `true` | Also use the NTP server the DHCP lease offers — usually the router, reachable with no internet. Restart required. |
| `time.sk_fallback` | `true` | Take the time from `navigation.datetime` while unsynced. |
| `time.tz` | `UTC0` | POSIX timezone, for displays only. |
| `sk.timestamps` | `true` | Put the measurement time on every delta. |

`time.from_dhcp` needs `CONFIG_LWIP_DHCP_GET_NTP_SRV=y`, which
`sdkconfig.d/espos.defaults` sets for every espOS firmware.

The SNTP keys are `restart_required`: re-arming a running client mid-poll is
more disruption than a reboot.

## Kconfig

| Symbol | Default | What it does |
|---|---|---|
| `CONFIG_ESPOS_TIME_RTC_STALE_H` | 24 | Hours before a deep-sleep-carried time stops counting as synced. 0 disables the rule. |
| `CONFIG_ESPOS_TIME_MAX_SUBSCRIBERS` | 4 | Slots for `espos_time_subscribe()`. |
| `CONFIG_ESPOS_LOG_WALLCLOCK` | y | Stamp stored log lines once the clock is known. |

## Where it sits

`espos_time` starts in `espos_start_network()` right after `espos_net_start()`
and before every transport, so the first `ESPOS_EVENT_NETWORK_UP` — whichever
transport produces it — is the one that starts SNTP polling. Arming the client
is not the same as starting it: a request sent before there is a route is a
wasted retry cycle.

It depends on `espos_config` and `espos_httpd`, and deliberately **not** on
`espos_sk`. The dependency runs the other way — `espos_sk` reads the clock for
its delta timestamps — so `espos_time` subscribing to a SignalK path would
close a cycle. Instead it offers `espos_time_set()` and `espos_sk` calls it
(`src/sk_time.c`): the source knows the clock, the clock knows nothing about
its sources.

`espos_log` and `espos_httpd` want the clock too and may not depend on it for
the same reason. Both declare a weak function that answers "there is no clock",
and `espos_time` overrides it when it is in the build. A firmware without the
component behaves exactly as it did before; one with it gets the clock, and
neither names the other.

The ranking, the staleness rule and the ISO 8601 conversion are pure C in
`time_policy.c` behind an injected clock, so `test/host/espos_time_test` can
step a day forward in one line.
