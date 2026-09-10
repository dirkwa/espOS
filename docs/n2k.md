# NMEA 2000 gateway

`espos_n2k` bridges an NMEA 2000 (CAN) bus to the network: a TWAI
receiver and transmitter, plus a TCP server that streams frames in
**candump** format so the bus is reachable from a laptop or a SignalK
server.

Frames are `espos_n2k::CanMessage` — espOS's own small struct, not the
driver's. That is what lets the candump codec be compiled and tested on the
host, and what keeps an IDF driver change from changing the type every
consumer names.

It is board-agnostic. The application chooses the pins and bitrate;
nothing here assumes a particular panel or transceiver.

```cpp
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/twai_transmitter.h"
#include "espos_n2k/candump_tcp_server.h"

static espos_n2k::TwaiReceiver rx({
    .tx_pin = GPIO_NUM_22, .rx_pin = GPIO_NUM_21, .bitrate = 250000});
static espos_n2k::TwaiTransmitter tx;
static espos_n2k::CandumpTcpServer srv(&rx, &tx, {.port = 2599});

rx.start();    // configures the bus: pins, bitrate
tx.start();    // joins the bus the receiver configured
srv.start();   // installs its own frame callback on the receiver
```

**Start the receiver first.** IDF 6's esp_twai allocates a *node* and hands
back a handle, where the old driver was a process-wide singleton any caller
could reach. The receiver owns that node — it is the one with the pins and
the bitrate — and the transmitter joins it. A transmitter started on its own
logs that the bus is not up and stays stopped, rather than dropping every
frame in silence the way it used to.

`start()` on the server wires itself to the receiver, so **do not call
`TwaiReceiver::set_on_frame()` afterwards** — it replaces the server's
callback and no frame reaches a client. Set your own callback before
starting the server if you also want frames in the application.

N2K is 250 kbit/s; the transceiver (SN65HVD230 or similar) is the
board's business, not this component's.

## When the bus is silent

`GET /api/v1/n2k` (register it with `espos_n2k_api_register(&receiver)`) is
the answer to "the candump socket connects and nothing arrives", which
otherwise needs a serial cable to diagnose:

```json
{ "present": true, "running": true, "ever_received": true, "idle_s": 0,
  "frames": 89559, "dropped": 0, "errors": 59, "bus_off": 0,
  "last_error": { "flags": 8, "stuff_err": true, ... } }
```

| Reading | Means |
|---|---|
| `running: false` | the driver never came up — pins, or a failed `twai_new_node_onchip` |
| `frames: 0` **and** `errors: 0` | the wire is electrically quiet: unplugged, unpowered, nobody transmitting — or the driver missed the bus, see below |
| `frames: 0`, `errors` climbing | the bus is live and not understood. `ack_err` alone = nothing else is listening; `stuff_err`/`form_err` = wrong bitrate |
| `dropped` climbing | frames arrive faster than they are consumed; raise `CONFIG_ESPOS_N2K_RX_QUEUE_DEPTH` |

**A bus connected after boot is not picked up until the device restarts.** A
device powered before its network stays deaf, and does not recover on its own
however long you wait. Whether that is IDF's TWAI driver or this component's
`start()` path is unproven; the evidence is in
[#15](https://github.com/signalk-espOS/espOS/issues/15).

This is the common case rather than an edge one: a device is routinely powered
before the network it listens to. **So on a silent bus, restart the device
before reaching for a multimeter** — and note that nothing raises an alarm for
it, deliberately, because a firmware may legitimately run with no N2K at all.

## Consuming the stream

The server advertises `_sensesp-n2k._tcp` over mDNS with
`format=candump3`, which SignalK's `n2k-ip-gateway-canboatjs` source
browses for. From a laptop the raw stream is readable directly:

```sh
nc <device> 2599
```

**The service type and the `model` TXT tag still say `sensesp-n2k`.**
They are on the wire and existing clients already browse for them;
renaming would make every deployed gateway invisible to every deployed
client, which is not worth tidiness.

## Migrating from the `driver/twai.h` version

* `TwaiMessage` is now `CanMessage`; `espos_n2k/twai_message.h` still
  defines the old name as an alias, so code that only *names* the type is
  unaffected. Code that reaches inside it is not: `identifier` →&nbsp;`id`,
  `data_length_code` →&nbsp;`dlc`, `extd` →&nbsp;`extended`, `rtr`
  →&nbsp;`remote`. The driver-only flags (`ss`, `self`, `dlc_non_comp`) are
  gone — they were a union's worth of bits nothing here ever set meaningfully.
* `TwaiTransmitter` no longer runs a task or a queue of its own: esp_twai
  queues internally, so `set()` hands the frame straight to the driver. It
  is still non-blocking and still drops (and counts) when the queue is full.
  `ever_transmitted()` now means "ever queued".
* Bus-off recovery moved from a poll in the RX loop to the driver's
  state-change callback, so it no longer waits for a receive timeout.

## Callbacks

`TwaiReceiver::set_on_frame()` takes a plain `std::function`, called on
the receiver's own task. Do not do slow work there and do not touch UI
state directly — copy what you need and hand it to the task that owns
it. The predecessor to this component inherited a SensESP observable
base class for the same job; the callback is the whole reason this code
no longer needs SensESP at all.
