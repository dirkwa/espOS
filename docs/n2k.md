# NMEA 2000 gateway

`espos_n2k` bridges an NMEA 2000 (CAN) bus to the network: a TWAI
receiver and transmitter, plus a TCP server that streams frames in
**candump** format so the bus is reachable from a laptop or a SignalK
server.

It is board-agnostic. The application chooses the pins and bitrate;
nothing here assumes a particular panel or transceiver.

```cpp
#include "espos_n2k/twai_receiver.h"
#include "espos_n2k/candump_tcp_server.h"

espos_n2k::TwaiReceiver rx({.tx_pin = 22, .rx_pin = 21, .bitrate_kbps = 250});
espos_n2k::CandumpTcpServer srv({.port = 2599, .interface_name = "can0"});

rx.set_on_frame([&](const espos_n2k::TwaiMessage& m) { srv.broadcast(m); });
rx.start();
srv.start();
```

N2K is 250 kbit/s; the transceiver (SN65HVD230 or similar) is the
board's business, not this component's.

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

## Callbacks

`TwaiReceiver::set_on_frame()` takes a plain `std::function`, called on
the receiver's own task. Do not do slow work there and do not touch UI
state directly — copy what you need and hand it to the task that owns
it. The predecessor to this component inherited a SensESP observable
base class for the same job; the callback is the whole reason this code
no longer needs SensESP at all.
