# Tutorial: logs and core dumps without a cable

**Advanced.** Everything the serial monitor shows is also on the device: a
ring of recent log lines, paged over REST and announced over SSE; the log
level, changeable at run time; and after a panic the core dump, downloadable
and decodable against your ELF. Start from any earlier tutorial's project and
build it with the debug profile, which enables the deliberate-crash endpoint
(`CONFIG_ESPOS_HTTPD_DEBUG_CRASH`) alongside heap poisoning and task
tracking — never ship it. Defaults apply only to a fresh `sdkconfig`, so start
the build directory over:

```sh
rm -rf build && espos/scripts/build.sh -DIDF_TARGET=esp32c6 -DESPOS_PROFILE=debug build
espos/scripts/build.sh -p /dev/ttyUSB0 flash
```

## 1. The log ring

`espos_start()` installs the ring before anything else logs, so the boot is
in it. `CONFIG_ESPOS_LOG_RING_SIZE` (16 KiB) of lines, colour codes stripped,
each truncated at `CONFIG_ESPOS_LOG_LINE_MAX` (256), oldest overwritten first,
every line numbered ([rest-api.md](../rest-api.md), "Logs"):

```sh
D=http://espos-xxxx.local/api/v1
curl -s "$D/logs?limit=20" | python3 -m json.tool
```

`lines[i]` has sequence `from + i`; `next` is the sequence the next line will
get; `first` and `dropped` say what the ring has lost; `gap` is true when your
`after` was older than what is still kept. To follow the log, ask for lines
after the last one you have. A `tail -f` in five lines of shell:

```sh
n=0
while sleep 2; do
  n=$(curl -s "$D/logs?after=$n" | python3 -c '
import json, sys; d = json.load(sys.stdin); print(*d["lines"], sep="\n", file=sys.stderr); print(d["next"] - 1)')
done
```

Polling is not how the UI does it: `curl -N $D/events` shows a `logs` event,
`{"next": 812}`, at most every 500 ms while lines arrive — published from a
timer, never from inside the logging call, so a slow browser cannot stall a
logger. The Logs page fetches `?after=` on each one and offers filter, follow
and download.

## 2. Log level at run time

```sh
curl -s -X PUT -H 'Content-Type: application/json' -d '{"level":"debug","tag":"espos_sk"}' $D/logs/level
curl -s -X PUT -H 'Content-Type: application/json' -d '{"level":"info","tag":"*"}' $D/logs/level
```

`esp_log_level_set()` over HTTP: `tag` defaults to `*`, `level` is one of
`none error warn info debug verbose`, not persisted. One catch that costs
people an hour: the compile-time ceiling. espOS's defaults set
`CONFIG_LOG_DEFAULT_LEVEL_INFO=y`, and `debug` lines are not compiled in
unless the maximum is raised — add `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y` to your
project's `sdkconfig.defaults` (flash for the strings, no run-time cost until
you turn a tag up) and rebuild. `espos_sk` at `debug` narrates every frame on
the stream, which is the fastest way to see what a server actually sends.

## 3. A core dump

espOS's defaults write a core dump to a 64 KiB `coredump` partition on every
panic (`CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`), and the task watchdog panics
a task that `espos_health_watch_task()` registered when it stops kicking for
30 s ([health.md](../health.md)). Cause one on purpose:

```sh
curl -s -X POST -H 'Content-Type: application/json' $D/system/crash   # debug profile only
```

The device panics, writes the dump, reboots; the monitor shows the panic and
the boot. When it is back:

```sh
curl -s $D/system/info | python3 -m json.tool | grep reset_reason         # "panic"
curl -s $D/system/coredump | python3 -m json.tool                           # present, task, pc, app_elf_sha256, …
curl -s -o coredump.bin $D/system/coredump/raw
espcoredump.py --chip esp32c6 info_corefile -c coredump.bin -t raw build/first_sensor.elf
idf.py coredump-info --core coredump.bin      # the same, from the project dir; chip and ELF come from build/
curl -s -X DELETE $D/system/coredump          # {"status": "erased"}
```

The summary alone names the task and the program counter; the decoded dump
gives the backtrace of every task with symbols. That needs the ELF of exactly
the build that crashed — `app_elf_sha256` in the summary identifies it — so
keep the `build/` directory (or at least the `.elf`) of every image you flash
or publish; a different build's ELF decodes into nonsense. The Status page
shows the same summary as "last crash" with download and erase buttons.

## 4. Three kinds of restart

`GET /system/info` tells them apart. A **panic** leaves `reset_reason
"panic"` and a core dump. A restart by the **health policy** — memory that
would not come back, a stalled task, a Signal K link dead for five minutes
over WiFi that claims to be up — leaves `reset_reason "software"` and a
`last_reset` record: the condition's key and message, the heap low-water
marks and how long that boot had run, valid for the whole boot that follows
([health.md](../health.md)). A **power cycle, an OTA reboot or `POST
/system/reboot`** leaves neither. The same reset reason is published to the
server as `espos.<hostname>.resetReason`, so a dashboard sees a device that
keeps restarting without anyone opening its logs.
