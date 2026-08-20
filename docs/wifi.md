# WiFi (`espos_wifi`)

Station connection manager with an explicit status model, a priority list of
networks, exponential backoff and a SoftAP provisioning portal. Configuration
is the `wifi` namespace (see the schema); status is `GET /api/v1/wifi/status`
and the `wifi` SSE event.

## State machine

```
            START (sta_enabled, ≥1 network)
                     │
     ┌───────────────▼────────────────┐   STA_CONNECTED   ┌──────────────┐  GOT_IP  ┌───────────┐
     │          CONNECTING            │──────────────────▶│ OBTAINING_IP │─────────▶│ CONNECTED │
     │  connect(net[i]); connect_tmo  │                   │  dhcp_tmo    │          │           │
     └───────────────┬────────────────┘                   └──────┬───────┘          └─────┬─────┘
        DISCONNECTED │ (reason) / timeout                DHCP timeout / drop         drop / LOST_IP
                     ▼                                          │                         │
          next network in the list? ──yes──▶ CONNECTING(i+1)   ◀────────────────────────┘
                     │ no (round complete)                (a drop from CONNECTED retries the
                     ▼                                     same network once before moving on)
     ┌────────────────────────────────┐
     │            BACKOFF             │  1 s · 2^round, capped at backoff_max_s, ±25 % jitter
     │  timer → CONNECTING(net[0])    │  round resets on success
     └────────────────────────────────┘

  DISABLED      sta_enabled=false: idle (portal may be up)
  UNCONFIGURED  no network has an SSID: idle, portal up immediately
```

Every driver event carries the raw reason code; the status exposes it as
`reason: {code, text}` with a human explanation:

| code | text                                   |
|------|----------------------------------------|
| 201  | network not in range (`NO_AP_FOUND`)   |
| 202  | wrong password (`AUTH_FAIL`)           |
| 2    | wrong password (auth expired)          |
| 15   | wrong password (4-way handshake timeout) |
| 204  | auth timed out, weak signal? (`HANDSHAKE_TIMEOUT`) |
| 200  | lost beacon (out of range or AP off?)  |
| 1001 | associated but no IP address (DHCP timeout) — **ours**, not a WiFi code |
| 1002 | no answer from the driver (connect timeout) — ours |
| 1003 | IP address lost — ours                 |
| 1004 | reconnecting after configuration change — ours |
| 1005 | station disabled — ours                |

The full table is in `wifi_reason.c`. Unknown codes read `unknown reason`
with the number alongside.

## Networks and priority

Four slots `ssid0..3` / `psk0..3` / `bssid0..3`. Slot order is priority; empty
slots are skipped. A round tries every configured network once (no backoff
in between); backoff applies between rounds. A live connection is **kept**
when the config changes as long as its network (SSID, password, BSSID pin)
still appears in the new list — reordering or adding networks never drops
the link; changing the password of the current network reconnects from the
top of the list. `bssidN` pins one access point (`aa:bb:cc:dd:ee:ff`) and
disables roaming for that slot.

Security: with a password set the station refuses open/WEP networks
(threshold WPA-PSK; WPA2 and WPA3-SAE/H2E are negotiated when the AP offers
them, PMF capable). Passwords must be 8..63 characters — shorter ones are
not valid WPA and the slot is skipped with a warning. Empty password = open
network.

## Portal (SoftAP provisioning)

The station keeps retrying while the portal is up — the portal never
replaces the station, it runs alongside (APSTA).

| Situation                          | Portal                                    |
|------------------------------------|-------------------------------------------|
| no network configured              | up immediately                            |
| `sta_enabled = false`              | up immediately                            |
| retrying without success           | up after `portal_after_s` (default 90 s)  |
| connected                          | down                                      |
| `portal_enabled = false`           | never                                     |

SSID `portal_ssid` (default `espOS-<last 4 hex of MAC>`), open unless
`portal_psk` (≥ 8 chars) is set. The AP is `192.168.4.1/24` with DHCP; a
tiny DNS responder answers every name with that address and the usual OS
probe URLs (`/generate_204`, `/hotspot-detect.html`, `/connecttest.txt`,
…) redirect to `/`, so phones pop their "sign in to network" sheet and land
on the setup page. The setup page scans, lets you pick a network and PUTs
`wifi.ssid0/psk0` through the normal API. Once connected the portal drops
(the phone loses the AP — expected).

## Provisioning without the portal

The store is plain NVS, so a factory image works: put the credentials in a
CSV **outside the repo** (patterns like `*.nvs.csv` are git-ignored),
generate the partition and flash it to the `nvs` offset:

```sh
printf 'key,type,encoding,value\nwifi,namespace,,\nssid0,data,string,MyBoat\npsk0,data,string,secret\n' > wifi.nvs.csv
python -m esp_idf_nvs_partition_gen generate wifi.nvs.csv wifi.nvs.bin 0x6000
esptool.py --port /dev/ttyACM0 write_flash 0x9000 wifi.nvs.bin
```

`config_version` need not be present — the store stamps it on first boot.

## Status document

`GET /api/v1/wifi/status`:

```json
{
  "state": "connected", "sta_enabled": true, "hostname": "espos-1a2b",
  "network_index": 0, "ssid": "Boat", "bssid": "aa:bb:cc:dd:ee:ff", "channel": 6, "rssi": -59,
  "ip": "192.168.1.23", "netmask": "255.255.255.0", "gateway": "192.168.1.1", "connected_s": 26,
  "reason": {"code": 0, "text": ""},
  "attempt": 0, "round": 0, "connect_count": 1, "disconnect_count": 0,
  "portal": {"active": false, "ssid": "espOS-1a2b"}
}
```

`ssid/bssid/channel/rssi` appear from `obtaining_ip` on, `ip/netmask/gateway/
connected_s` only in `connected`, `backoff_ms` only in `backoff`, `portal.ip`
and `portal.clients` only while the portal is active. The same document is
pushed as the `wifi` SSE event on every change (and once on connect).

## Co-processor link watchdog (esp_hosted boards)

On boards where the radio lives on a second chip — ESP32-P4 host with an
ESP32-C6 over SDIO, and similar — the host↔co-processor transport can
wedge while the WiFi state machine still reports `connected`. The host
log fills with

```
rpc_core: Timeout waiting for Resp for [0x126](Req_WifiStaGetApInfo)
```

and nothing else: the RPC channel is gone, so the host cannot even ask
whether it is associated. Upstream tracks this as
[esp-hosted-mcu#197](https://github.com/espressif/esp-hosted-mcu/issues/197)
and [#220](https://github.com/espressif/esp-hosted-mcu/issues/220).

esp_hosted raises `ESP_HOSTED_EVENT_TRANSPORT_FAILURE` for faults it can
detect itself (a dropped SDIO read, an all-ones `PKT_LEN`), and with
`CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE=y` — the default, which
espOS keeps — that reboots the device. **A silently wedged link raises no
event at all**, because there is nothing to detect, only an absence.

`espos_wifi_hosted_watchdog_start()` supplies the missing signal. It
enables the co-processor heartbeat (20 s) and watches for it. The
heartbeat travels over the same RPC channel that dies, so its absence
*is* the fault detector. After three missed beats the device restarts.

It starts automatically from `espos_wifi_start()`; there is nothing to
call. On native-radio and simulator builds it compiles to a stub
returning `ESP_ERR_NOT_SUPPORTED`. A non-OK return means wedges will not
be *detected* — not that WiFi is broken.

### Why a restart and not a transport re-init

The first version of this called `esp_hosted_deinit()` →
`esp_hosted_init()` → `esp_hosted_connect_to_slave()`, to recover in
seconds while keeping config and UI state. That is what the upstream
`host_hosted_events` example does, and it looked strictly better than a
reboot.

It is not, because that pair **asserts rather than returning an error**
when it cannot re-allocate:

```text
assert failed: sdio_mempool_create sdio_drv.c:258 (buf_mp_g)
```

`sdio_mempool_destroy()` runs on deinit and `sdio_mempool_create()` on
init; if the pool cannot be re-allocated — fragmentation, or the old one
not fully released — the assert fires on whatever task called it. On a
panel that was the `esp_timer` task, so the "graceful recovery" panicked
the system. Error handling on the return value cannot help: the assert
fires first.

The transport is already broken when recovery runs, so the recovery path
must not have a failure mode of its own. `esp_restart()` always works.
A device that restarts 60 s after its radio link dies is strictly better
than one sitting unreachable until someone power-cycles it, which is the
behaviour this replaces — and that, not the in-place repair, was always
the valuable half.

## Design notes

* The state machine (`wifi_sm.c`) is pure C over an injected port and runs
  unchanged on the host (`test/host/espos_wifi_test`, every transition
  above). The device port (`port_idf.c`) is esp_wifi/esp_netif glue; the
  host port (`port_sim.c`) scripts a fake driver so the REST/SSE layer is
  testable too.
* Driver calls are **never made under the state-machine lock**. The SM
  queues actions; `espos_wifi_dispatch()` runs them after unlocking. On
  ESP32-P4 the driver is a co-processor behind an RPC that itself delivers
  events into the SM — calling it under the lock deadlocked within a
  second of boot.
* One FreeRTOS timer serves connect/DHCP/backoff timeouts and the portal
  deadline (whichever is earlier), so there is nothing to keep in sync.
* ESP32-P4: WiFi is an ESP32-C6 co-processor over SDIO (`esp_hosted` +
  `esp_wifi_remote`, P4-only dependencies in `main/idf_component.yml`;
  pinout in `sdkconfig.defaults.esp32p4`). Same `esp_wifi_*` API; the MAC is
  read from the driver, not eFuse.

  Two settings on that transport are load-bearing, both pinned in
  `sdkconfig.defaults.esp32p4`:

  * **`CONFIG_WIFI_RMT_RX_BA_WIN=6`.** IDF defaults this to 6 but raises it
    to 16 as soon as a project enables PSRAM
    (`SPIRAM_TRY_ALLOCATE_WIFI_LWIP`) — which any board with a display
    will. At 16 the SDIO Rx path overruns under sustained inbound TCP: the
    link wedges, `H_SDIO_DRV` spams *"task still writing Rx data to
    queue!"*, every RPC to the C6 times out, and WiFi stays dead until the
    host reboots — while the state machine still reports `CONNECTED`,
    because the disconnect event never crosses the jammed link
    (espressif/esp-hosted-mcu#184). Measured on a Waveshare 7B with
    repeated ~30 KB HTTP reads: wedged after 85 requests at 16, survived
    400 at 6. Note the knob is `WIFI_RMT_*` — with hosted WiFi the radio is
    remote, so the local `ESP_WIFI_RX_BA_WIN` does not reach it.
  * **`CONFIG_ESP_HOSTED_SDIO_OPTIMIZATION_RX_STREAMING_MODE=y` must stay
    on.** The C6 slave firmware is fixed in streaming mode and the host has
    to match, or the transport asserts at boot: *"SDIO mode mismatch: slave
    is in streaming mode, but host is in packet mode. Aborting."*

  Since a wedged transport cannot be recovered from the host (there is no
  reconnect API, and the RPC that would carry one is exactly what times
  out), an application that must survive it unattended should reboot on
  its own liveness signal — real traffic, not `ESPOS_WIFI_ST_CONNECTED`,
  which keeps reporting success.
* Not yet: BLE provisioning (`espressif/network_provisioning`, planned as a
  follow-up), country code, static IP.
