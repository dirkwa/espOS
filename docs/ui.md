# Web UI (`ui/`) — M5

A Preact + TypeScript single-page app built with Vite, served by
`espos_httpd` from the LittleFS `storage` partition. It talks only to the
versioned REST API in [api.md](api.md) and gets live state over the SSE
stream — no polling, no coupling to firmware internals.

Pages: **Status** (WiFi, SignalK, device, last crash), **WiFi** (join /
scan / saved networks / portal), **SignalK** (token state and actions,
delta stream, discovered servers, manual server), **Config** (every
namespace rendered from `GET /config/schema`: types, ranges, enums,
secrets, restart-required marker, export/import JSON, reset section),
**Logs** (live log ring with filter, follow, download, runtime log level),
**OTA** (placeholder until M6).

## Working on it — no hardware needed

```sh
cd ui
npm ci
npm run dev            # http://localhost:5173, API mock started in-process
ESPOS_API=http://192.168.0.118 npm run dev     # proxy /api to a real device…
ESPOS_API=http://127.0.0.1:<port> npm run dev  # …or to the host harness (test/host/espos_httpd_test)
```

`mock/server.mjs` (node, zero deps) implements the API contract with a
simulated WiFi state machine, discovery + token flow, a log ring and SSE,
and regenerates the config schema from the real descriptors via
`tools/espos_gen_config.py` when python3 is present. It is the reference
"device" for UI development; when the API changes, change the mock and the
docs together.

`npm run build` type-checks (strict, `noUncheckedIndexedAccess`), bundles
(~18 KiB gzipped in total) and writes `dist-gz/` — every file gzipped as
`<name>.gz`, nothing else. The root `CMakeLists.txt` turns `ui/dist-gz`
into `build/storage.bin` (`littlefs_create_partition_image`,
`FLASH_IN_PROJECT`), so `idf.py flash` writes it. Without a UI build the
firmware still builds and serves the embedded placeholder page (`GET
/system/info` → `ui_storage`, and the boot log says so).

## Serving rules (firmware side)

* `<path>.gz` first, sent with `Content-Encoding: gzip` regardless of
  `Accept-Encoding` (all browsers accept it; `curl --compressed`).
* `/assets/*` is content-hashed → `Cache-Control: immutable` for a year;
  `index.html` → `no-cache`, so a new bundle is picked up on reload.
* Unknown extension-less paths → `index.html` (SPA routing with
  `history.pushState`); unknown files with an extension → JSON 404.
* Static serving is the 404 fallback for `GET`, not a wildcard handler, so
  API handlers registered later by other components are never shadowed.
* `..` and `//` in a path are refused.

## Design notes

* State lives in tiny subscribable stores fed by one `EventSource`
  (`src/api.ts`); pages `useStore()` what they show. `EventSource`
  reconnects on its own (`retry: 3000`), the header shows the link state.
* No component library, no router package: a 30-line history router and
  ~120 lines of CSS with light/dark via `prefers-color-scheme`.
* The Config page is generic: adding a key to a descriptor JSON adds a
  field. Secrets render as set/not-set with Set/Change/Clear (the sentinel
  `********` is never sent back as a value). Numbers/enums/booleans get the
  matching control; `x-espos-unit`, ranges and `maxLength` are shown as
  hints; `x-espos-restartRequired` keys carry ↻ and a save of one offers a
  reboot.
* Destructive actions (reboot, factory reset, forget network/token,
  erase core dump, reset section) confirm first.

## Log ring (`espos_log`)

`espos_log_init()` hooks `esp_log_set_vprintf`: each console line is also
kept in a byte ring (`CONFIG_ESPOS_LOG_RING_SIZE`, default 16 KiB) as
`[u16 len][text]`, colour codes stripped, sequence-numbered, oldest
overwritten first; the previous vprintf still runs, so the console is
unchanged. Log v2's prefix/message/newline calls are gathered into one
record; a message longer than `CONFIG_ESPOS_LOG_LINE_MAX` is truncated
and closed. A FreeRTOS timer (500 ms) publishes the `logs` SSE event when
new lines arrived — never from inside the logging call, so nothing can
recurse or block a logger on a slow SSE client. `main.c` calls it before
anything else to catch the boot log; `espos_httpd_start()` calls it too
(idempotent).
