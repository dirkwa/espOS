# Concepts

What espOS is made of, in what order it comes up, which task calls you back,
and how a setting travels from a JSON descriptor to the web UI. The
per-component documents ([config](config.md), [wifi](wifi.md),
[signalk](signalk.md), [ota](ota.md), [health](health.md), [REST
API](rest-api.md)) go deeper; this one is the map.

## Component graph

```
                 application (main/)
                        │
                   espos_core            espos_start(): the order below, once
                        │
   ┌────────┬───────────┼───────────┬────────────┬───────────┐
espos_log  espos_config  espos_health  espos_httpd  espos_wifi  espos_event
                                          │            │
                                          └─────┬──────┘
                                             espos_sk                 optional, started if built
                                                │
                                            espos_ota                 optional
                                            espos_ble                 optional
                       espos_n2k · espos_voice · espos_audio          optional, application-started
```

Arrows point at what a component needs. `espos_event` is a leaf: it needs
nothing of espOS, so anything may post to it without creating a cycle.
`espos_health` is likewise a leaf so that raising a condition never drags the
SignalK stack in ([health.md](health.md)). `espos_n2k`, `espos_voice` and
`espos_audio` have no start order of their own; an application starts them
when its board has the hardware.

## espos_start(): the order and why

```c
#include "espos.h"
void app_main(void) { ESP_ERROR_CHECK(espos_start(NULL)); /* your application */ }
```

| Step | Call | Why here |
|---|---|---|
| 1 | `espos_log_init()` | The ring must exist before the first line worth keeping; `/api/v1/logs` shows the boot. |
| 2 | `espos_config_init()` | Everything after this reads its settings from the store. Posts `ESPOS_EVENT_CONFIG_READY`. |
| — | `before_network` hook | Display up, so it can show the portal SSID; anything that must exist before a client can connect. |
| 3 | `espos_httpd_start()` | Before WiFi: the provisioning portal's page has to be there the moment the access point is. Posts `HTTPD_STARTED`. |
| 4 | `espos_wifi_start()` | Station, portal, `/wifi` endpoints. Posts `NETWORK_UP` / `NETWORK_DOWN` as the station link comes and goes. Brings up the mDNS responder (`<hostname>.local`, `_http._tcp`, `_espos._tcp`, [wifi.md](wifi.md)); `MDNS_READY` follows every `NETWORK_UP`. |
| 5 | `espos_sk_start()` (if built) | Discovery is mDNS and the stream needs the station; polls the WiFi status, so WiFi must exist. |
| 6 | `espos_ota_start()` (if built) | Its API sits on the HTTP server; its confirm/rollback policy watches WiFi. |
| 7 | `espos_ble_start()` (if built) | Authenticates with the SignalK token. |

`espos_init()` is steps 1–2, `espos_start_network()` steps 3–7; both are
idempotent, as is every `espos_*_start()`. "If built" is decided at
configure time: `espos_core` links `espos_sk`, `espos_ota` and `espos_ble`
only when they are in the project's component list, so a firmware picks its
stacks by listing components, not by editing a start sequence.

The order is also enforced at runtime. Each start checks its prerequisite
and fails with `ESP_ERR_INVALID_STATE` plus one log line naming the missing
call — `espos_wifi_start: call espos_httpd_start() first (or espos_start())`:

| Call | Requires |
|---|---|
| `espos_httpd_start()` | `espos_config_is_ready()` |
| `espos_wifi_start()` | `espos_httpd_handle() != NULL` |
| `espos_sk_start()` | WiFi started (`espos_wifi_get_status()` answers) |
| `espos_ota_start()` | HTTP server and WiFi started |

A device coming up narrates itself on the monitor in this order: `espOS
<version> on <target> — app <name> <version>`; `no network configured: join
"<portal ssid>" and open http://192.168.4.1` (or `connected to "<ssid>" as
<ip> — web UI: http://<hostname>.local`); `found signalk-server "<name>" at
<host>:<port>`; `access requested — approve it in the server UI: Security →
Access Requests`; `approved, streaming`.

## Threading contracts

Every callback in espOS runs on a task that is not yours. The rule is the
same everywhere — copy what you need, return quickly, never block on
something that may itself be waiting for the task you are on — but the task
differs, and it matters when you reach for a lock:

| Callback | Runs on | Notes |
|---|---|---|
| `espos_config_subscribe` change callback | the writer's task (an HTTP handler, usually) | Store lock released; the callback may read or write config. |
| `espos_sk_subscribe` update callback | the SK stream task | Strings valid only during the call. Never call an `espos_sk_*` that could wait on the stream. |
| `espos_sk_put` result callback | the SK stream task | Same rules. |
| `espos_health_add_sink` sink | the reporting task | No lock held; a sink may report conditions of its own. |
| `espos_sk_http_*`, `espos_sk_get_value/meta` | the caller | **Blocks** up to 2×timeout_ms; not from the stream task, an event handler or a URI handler. |
| `espos_mdns_start/add_service/remove_service` | the caller | **May block** a few ms on the responder task; not from an event or URI handler. |
| `espos_health` policy tick | the `esp_timer` task | Every 10 s: reports `lowMemory`/`taskStalled`, counts strikes, restarts; sinks run there on that tick. |
| HTTP URI handlers (`espos_httpd_register`) | the `esp_http_server` task | One task for all requests; a slow handler stalls the UI. |
| `ESPOS_EVENT` handlers | the default esp_event loop task | Shared with `WIFI_EVENT`/`IP_EVENT`; a blocked handler stalls the WiFi driver's own events. |
| `espos_log_read` visitor | the caller, with the ring locked | Do not log from inside. |
| `espos_wifi_refresh_rssi()` | the caller | **May block** on a co-processor RPC; not from a UI or event task. |

Publishing is the other way round: `espos_sk_publish_*`, `espos_sk_notify`,
`espos_health_report` and `espos_event_post` are thread-safe and never block
for long, so they are safe from any task, including the callbacks above.

## Descriptor → keys → UI

A setting exists in exactly one place: a JSON descriptor per NVS namespace,
registered by the component that owns it
(`espos_config_add_descriptor(config/myns.json)` in its `CMakeLists.txt`).
At build time the generator turns every registered descriptor into

* `espos_cfg_keys.h` — `ESPOS_CFG_NS_<NS>` and `ESPOS_CFG_<NS>_<KEY>`
  constants, so code never spells an NVS key by hand;
* the C tables `espos_config` validates against at runtime — types, ranges,
  defaults, `secret`, `restart_required`;
* the JSON Schema served at `GET /api/v1/config/schema`, from which the web
  UI renders its forms without knowing any component.

So a new setting is one JSON entry: the key constant, the validation, the
REST document and the settings page all follow. A project that registers no
descriptor of its own still builds (empty tables, an empty schema).
Details, migrations and the descriptor format: [config.md](config.md).

## Events

`ESPOS_EVENT` on the default esp_event loop; subscribe with
`espos_event_subscribe(id, handler, arg)`, `ESP_EVENT_ANY_ID` for all.

| Event | Posted by | Data |
|---|---|---|
| `ESPOS_EVENT_CONFIG_READY` | `espos_init()` after the store is up | — |
| `ESPOS_EVENT_HTTPD_STARTED` | `espos_httpd_start()` | — |
| `ESPOS_EVENT_NETWORK_UP` | `espos_wifi`, station got an IP | `espos_event_network_t {ip, hostname}` |
| `ESPOS_EVENT_NETWORK_DOWN` | `espos_wifi`, station link lost (one per UP) | — |
| `ESPOS_EVENT_MDNS_READY` | `espos_wifi` (mdns.c): on every `NETWORK_UP` once the responder runs, and once from `espos_mdns_start()` if the link is already up | — |
| `ESPOS_EVENT_SK_SERVER_SELECTED` | `espos_sk`, a server was chosen or changed | `espos_event_sk_server_t {host, port}` |
| `ESPOS_EVENT_SK_TOKEN_APPROVED` | `espos_sk`, token verified | — |
| `ESPOS_EVENT_SK_STREAM_CONNECTED` / `_DISCONNECTED` | `espos_sk` stream task, on every transition | — |
| `ESPOS_EVENT_OTA_AVAILABLE` | `espos_ota`, manifest names a newer build | `espos_event_ota_t {version}` |

Events are notifications, not state: a subscriber that comes late asks the
component's status API (`espos_wifi_get_status()`, `espos_sk_get_server()`,
`espos_ota_status_json()`) for the current picture and uses events to learn
about changes from then on. Handlers run on the event loop task (see above).
Ids are part of the ABI and are only ever appended.
