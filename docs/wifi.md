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

Security: WPA2-PSK minimum when a password is set (no downgrade to open/WEP),
WPA3-SAE (H2E) accepted when the AP offers it, PMF capable. Empty password =
open network.

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
  "state": "connected", "sta_enabled": true, "hostname": "espos-cbec",
  "network_index": 0, "ssid": "MOIN", "bssid": "1c:0b:8b:90:da:90", "channel": 6, "rssi": -59,
  "ip": "192.168.0.118", "netmask": "255.255.255.0", "gateway": "192.168.0.1", "connected_s": 26,
  "reason": {"code": 0, "text": ""},
  "attempt": 0, "round": 0, "connect_count": 1, "disconnect_count": 0,
  "portal": {"active": false, "ssid": "espOS-cbec"}
}
```

`ssid/bssid/channel/rssi` appear from `obtaining_ip` on, `ip/netmask/gateway/
connected_s` only in `connected`, `backoff_ms` only in `backoff`, `portal.ip`
and `portal.clients` only while the portal is active. The same document is
pushed as the `wifi` SSE event on every change (and once on connect).

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
* Not yet: BLE provisioning (`espressif/network_provisioning`, planned as a
  follow-up), country code, static IP.
