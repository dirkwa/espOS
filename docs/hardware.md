# Hardware

What espOS runs on, what differs between the chips, and what to expect from
the boards Signal K users already own. Board-specific code — display HALs,
audio codecs, pin maps — stays in the application; espOS only needs a chip
it builds for and a radio.

## Chips

CI builds the reference application for five targets on every change
(`.github/workflows/ci.yml`); every example builds for `esp32c6` and the ones
with peripherals for `esp32p4` too.

| Target | Status | What to know |
|---|---|---|
| `esp32` | in CI | Native radio, Bluedroid BLE. Needs **chip revision 3.0 or later** for the RSA app-signing scheme ([OTA](ota.md)); `sdkconfig.d/espos.defaults.esp32` pins `ESP32_REV_MIN_3`, and every module sold since 2020 (WROOM-32E/UE, WROVER-E) qualifies. The monitor's boot banner prints the revision. |
| `esp32s3` | in CI | Native radio. With `esp32p4` the only target esp-sr supports, so the only other one that can run the voice satellite's wake word. |
| `esp32c3` | in CI | RISC-V, single core, the smallest. Native radio. |
| `esp32c6` | in CI | RISC-V, WiFi 6, native radio. The target the getting-started guide and the examples use. |
| `esp32p4` | in CI, in daily use | **No radio of its own.** WiFi and BLE come from an ESP32-C6 co-processor over SDIO (below). PSRAM; internal RAM is the scarce pool ([health](health.md)). Rev 1.x silicon allowed. |
| `esp32c5`, `esp32c61` | later | Same shape as the C6; waiting for hardware on the bench and a CI slot. |
| `esp32h2`, `esp32h4` | not planned as such | No WiFi radio (BLE + 802.15.4 only). The runtime no longer assumes WiFi -- [`espos_net`](net.md) is the seam and a headless esp32h2 build is a CI gate -- so what these still need is a transport (Thread, or Ethernet on a board that has it). |

The toolchain is one ESP-IDF for all of them: 6.0.x, tested on the release in
`.idf-version` ([Development](development.md)).

## Flash size and partition tables

The bundled tables have the same layout and differ in the app slots and the
storage partition; the prologue's `PARTITIONS` argument picks one and sets
the flash size with it (`espos_project_prologue(PARTITIONS "${ESPOS_DIR}/partitions/8mb.csv")`).

| Table | Flash | App slots (`ota_0`, `ota_1`) | `storage` (LittleFS: web UI + your files) | Fits |
|---|---|---|---|---|
| `partitions/4mb.csv` (default) | 4 MB | 2 × 1600 KB | 640 KB | the core, Signal K, OTA — the getting-started device |
| `partitions/8mb.csv` | 8 MB | 2 × 3 MB | 1792 KB | a native-radio target with Bluedroid: the BLE gateway links about 2 MB, which does **not** fit a 1600 KB slot |
| `partitions/16mb.csv` | 16 MB | 2 × 6656 KB | 1792 KB | LVGL + esp-sr + hosted WiFi/BLE on the P4 panels; 1 MB left for the esp-sr model partition (commented out in the CSV) |

All three carry `nvs` (24 KB — configuration, WiFi credentials, the Signal K
token), `otadata`, `phy_init`, `nvs_keys` (for NVS encryption in a release
build, [Security](security.md)) and a 64 KB `coredump` partition. A project
with its own table sets `CONFIG_ESPTOOLPY_FLASHSIZE_*` in its own defaults.
`esptool.py flash_id` reports the flash size of a board you are unsure about.
CI's size report fails the build when the app grows past 90 % of its slot
(`tools/espos_size_diff.py`).

## ESP32-P4 and the C6 co-processor

The ESP32-P4 has no radio. On the boards espOS is developed on — the
**Waveshare ESP32-P4-WIFI6-Touch-LCD-7B** (7", 1024 × 600) and **-4B** (4",
720 × 720) panels — an ESP32-C6 module provides WiFi 6 and BLE over SDIO
through Espressif's `esp_hosted` transport and the `esp_wifi_remote` shim, so
the application still calls the ordinary `esp_wifi_*` API. Both panels carry
P4 rev 1.x silicon, 16 MB flash, 32 MB PSRAM and a GT911 touch controller;
they differ in the display panel, which is the application's HAL, not
espOS's.

Everything espOS knows about that transport is in
`sdkconfig.d/espos.defaults.esp32p4`, which every P4 build inherits:

* **SDIO pinout** for the Waveshare family: slot 1, 4-bit bus, `CLK` 18,
  `CMD` 19, `D0..D3` 14–17, C6 reset on GPIO 54 (active high), 40 MHz.
  Another P4 board changes these lines in its own `sdkconfig.defaults`.
* **Three load-bearing settings**, explained in [WiFi → Design
  notes](wifi.md#design-notes): the receive block-ack window
  `CONFIG_WIFI_RMT_RX_BA_WIN=6` (IDF raises it to 16 when PSRAM is on and the
  SDIO Rx path then wedges under sustained inbound TCP), the transport
  mempool in PSRAM with **64-byte L2 cache lines** (the 1600-byte stride is
  not 128-aligned; the two options are pinned together), and SDIO streaming
  mode (the stock C6 firmware is fixed in it).
* `CONFIG_ESP32P4_REV_MIN_100` for the rev 1.x silicon, an 8 KB main task
  stack, `FREERTOS_HZ=1000`.

The co-processor needs no reflashing: a stock C6 slave reports
`capabilities: 0xd` — WLAN and BT over SDIO, BLE only — which is what the BLE
gateway uses (HCI over SDIO, Bluedroid host on the P4). Two limits of that
path, from [BLE gateway → Targets](ble.md#targets): **BLE 4.2 only** (the
slave does not forward BLE 5.0 extended HCI commands correctly) and **passive
scan** by default (active scan over SDIO is unreliable). The C6-MINI-1U
module on these panels has **no PCB antenna**: with nothing on its IPEX
connector the radio hears nothing at all.

When the SDIO link wedges, the WiFi state machine still reports connected
because the disconnect never crosses the jammed link. espOS's answer is the
co-processor heartbeat watchdog (`espos_wifi_hosted_watchdog_start()`, armed
by `espos_wifi_start()`: three missed 20 s beats restart the device) and the
`skLinkStalled` health condition; an in-place `esp_hosted_deinit()`/`init()`
recovery is **not** attempted, because it asserts instead of failing
([WiFi → Co-processor link watchdog](wifi.md#co-processor-link-watchdog-esp_hosted-boards)).

Flashing is `idf.py -p /dev/ttyACM0 flash` over the USB port as on any
target; the P4's USB-Serial/JTAG console on that same port is also the only
place a panic backtrace appears.

## Boards SensESP users own: SH-ESP32 and HALMET

Hat Labs' **SH-ESP32** and **HALMET** are the boards most SensESP projects
run on. From espOS's point of view they are plain ESP32 (original) boards
with a CAN transceiver and a wide-range power supply — the `esp32` target,
nothing special to configure:

* **Target and revision.** `idf.py set-target esp32`. The module on these
  boards is a current WROOM/WROVER, so the revision-3 requirement for signed
  images is met; the boot banner (`chip revision: v3.x`) confirms it.
* **Flash size** decides the partition table: `esptool.py flash_id` on the
  board tells you whether `4mb.csv` (the default) or `8mb.csv` applies. A
  WROVER module adds PSRAM; a WROOM has none, and none of the core needs it.
* **NMEA 2000.** The transceiver is on the board, so `espos_n2k` is a
  receiver, a transmitter and the pins ([NMEA 2000 gateway](n2k.md)); the
  bitrate is 250 kbit/s. On the SH-ESP32 the CAN transceiver is wired to
  **GPIO 34 (RX)** and **GPIO 32 (TX)** — the pins SensESP's SH-ESP32 examples
  use — so:

  ```cpp
  static espos_n2k::TwaiReceiver rx({.tx_pin = GPIO_NUM_32, .rx_pin = GPIO_NUM_34, .bitrate = 250000});
  ```

  HALMET routes its transceiver differently; take the pins from the HALMET
  firmware's board constants rather than from this page.
* **Everything else** SensESP did on these boards — 1-Wire temperature,
  I²C sensors, the HALMET's analog and digital inputs — is application code
  on top of `espos_sk_publish_*`; the [examples](examples.md) `analog_input`
  and `pulse_counter` are the starting points, and [Migrating from
  SensESP](migration-from-sensesp.md) has the mapping.
* **BLE.** The BLE gateway on an original ESP32 links Bluedroid and needs the
  8 MB table (above).

## Before you buy

Any development board with one of the five chips works for the core:
a bare ESP32-C6 module on a breakout is the cheapest way to try espOS, and a
Waveshare P4 panel is the reference for anything with a display or voice.
For a co-processor board other than Waveshare's, budget a session for the
SDIO pinout and the three settings above; for a board with an external
antenna connector, budget an antenna.
