# BLE gateway

`espos_ble` bridges Bluetooth Low Energy devices to
[signalk-server](https://github.com/SignalK/signalk-server)'s BLE provider
API. Optional: it only enters the build when the component is present and
Bluetooth is enabled in sdkconfig.

## What it is not

The gateway is a **dumb, stateless bridge**. It does not decode sensors, does
not name SignalK paths, does not publish deltas, and holds no device model.
Raw advertisements and GATT bytes go to the server; signalk-server (with
`bt-sensors-plugin-sk`) owns every decision about what a device is, which
characteristics to read, and how the values map to SignalK.

That is why there is almost nothing to configure here. Which devices to talk
to and how arrives at runtime, as `gatt_subscribe` commands from the server.
A gateway that is reflashed keeps working the moment it reconnects, because
none of that state was ever its own.

## Two channels

Both authenticate with the token `espos_sk` already holds — the gateway never
runs an access request of its own.

```text
POST http://<server>/signalk/v2/api/ble/gateway/advertisements
     Authorization: Bearer <jwt>
     {"gateway_id","mac","uptime","free_heap","devices":[{"mac","rssi","name?","adv_data?"}]}

WS   ws://<server>/signalk/v2/api/ble/gateway/ws?token=<jwt>
     out: hello, status, gatt_connected, gatt_disconnected, gatt_data, gatt_error
     in:  hello_ack, gatt_subscribe, gatt_write, gatt_close
```

Wire-format details that are contract, not taste (see signalk-server's
`packages/server-api/src/typebox/ble-schemas.ts`):

* keys are snake_case;
* advertisement `adv_data` is **UPPERCASE** hex while GATT `data` is
  **lowercase** — two different encoders, deliberately;
* the JWT rides in the WebSocket **query string**, because a raw upgrade
  request carries no `Authorization` header;
* an empty token means the `Authorization` header is **omitted entirely**,
  which is correct against a server running without security.

## GATT writes: `with_response`

Every write in `init[]` and the direct `gatt_write` command may carry an
optional `with_response` flag. (`periodic_write[]` is part of the server-side
protocol but is **not implemented here yet**; a server that sends it gets no
writes.) **Absent means
write-with-response**, matching the server-side default and the behaviour of
every gateway that predates the flag.

It matters because some peripherals — JK-BMS and Daly-BMS among them — reject
write-with-response on their command characteristic with a GATT "Write not
permitted" and accept only write-without-response.

The subtlety worth knowing: a write-without-response generates **no
completion event**. Anything that sequences writes must not wait for one.
This implementation handles that in two places — `ble_gattc.c` synthesises the
callback the stack will never send, and the gateway additionally puts a 3 s
deadline on each init write. Without both, honouring the flag would leave a
session stuck in initialisation forever: never subscribing, never reporting,
never resuming the scan.

## Configuration

Namespace `ble` (see `components/espos_ble/config/ble.json`); everything is
editable in the web UI. Note NVS caps key names at 15 characters, which is
why they read `scan_int_ms` rather than `scan_interval_ms`.

| key | default | notes |
|-----|---------|-------|
| `enabled` | `true` | restart required |
| `active_scan` | `false` | see the P4 warning below |
| `scan_int_ms` / `scan_win_ms` | 320 / 160 | window ≤ interval; the ratio is the duty cycle |
| `post_int_ms` | 2000 | how often buffered advertisements are forwarded |
| `status_int_ms` | 30000 | status frame cadence on the control WS |
| `max_pend_ads` | 500 | ring buffer depth; oldest dropped when full |
| `control_ws` | `true` | off = advertisements only, which is all beacons need |
| `max_gatt_sess` | 3 | Bluedroid's own ceiling |

## Targets

One backend covers both cases; only the controller bring-up differs.

**Native Bluedroid** (ESP32, C3, S3, C6): the chip's own radio.

**ESP32-P4**: no radio at all. Bluedroid's HCI is routed at an ESP32-C6
co-processor over esp_hosted's SDIO transport
(`CONFIG_ESP_HOSTED_ENABLE_BT_BLUEDROID` + `..._HCI_VHCI`, with
`CONFIG_BT_CONTROLLER_DISABLED`). Three things about that path are easy to
get wrong:

1. **Order is load-bearing.** The remote controller must be initialised *and
   enabled* before `esp_bluedroid_attach_hci_driver()`, because enabling it is
   what populates the driver's function pointers. Attaching first faults on
   the first call through them, and the `BT_HCI: command_timed_out opcode:
   0xc03` (HCI_Reset) that follows is a symptom of the host crash, not an
   independent fault.
2. **BLE 4.2, not 5.0.** The C6 slave's HCI bridge does not correctly forward
   BLE 5.0 extended HCI commands over SDIO; legacy scan is the working path.
3. **Passive scan.** Active scan needs the C6 to transmit SCAN_REQ over SDIO,
   which is unreliable and can silence advertisements entirely
   (esp-hosted-mcu#180). `active_scan` defaults to false for this reason.

A stock C6 slave already reports `capabilities: 0xd` = WLAN_SDIO + BT_SDIO +
BLE_ONLY, i.e. HCI over SDIO is present. There is nothing to reflash on the
co-processor. If the radio sees nothing at all while HCI is alive, suspect the
antenna: the C6-MINI-1U module has no PCB antenna and needs an external 2.4
GHz one on its IPEX connector.

## Status and troubleshooting

`GET /api/v1/ble/status` (and the `ble` SSE event) report the counters
described in [api.md](api.md). Reading them:

* `scan_hits` == `adv_received` — intake is keeping up.
* `adv_dropped` rising — the radio is outrunning the POST loop. Shorten
  `post_int_ms` or raise `max_pend_ads`. The count is exact.
* `post_fail` rising with `ws_connected: false` — a server or token problem,
  not a BLE one; check `GET /api/v1/sk/status` first.
* `scanning: false` while `enabled: true` — the controller refused to start;
  the boot log carries the reason.

## Host tests

`test/host/espos_ble_test` runs the wire-format logic on the linux target
(hex encoders, the advertisement ring and its drop accounting, UUID parsing
and endianness, `with_response` defaulting):

```sh
cd test/host/espos_ble_test
idf.py --preview set-target linux && idf.py build
./build/espos_ble_test.elf
```
