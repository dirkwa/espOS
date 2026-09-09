// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// candump_decode() and candump_resync_offset() — the NMEA 2000 gateway's
// text protocol, straight off a TCP socket.
//
// A client connects to the candump server and can send whatever it likes;
// decode() turns that into a CAN frame that goes on the boat's bus. That is
// the shortest path in this firmware from an arbitrary byte to something
// physical, which is why it is fuzzed even though the codec is small.
//
// resync_offset() is here for a different reason: it was written days ago to
// fix a partial-send bug, its unit tests found a boundary error in the first
// implementation, and the property it must hold -- whatever it keeps starts a
// line -- is exactly the kind a fuzzer checks better than examples do.
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "espos_n2k/can_frame.h"
#include "espos_n2k/candump_format.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  // ── decode: a NUL-terminated line, as the server hands it over ──────────
  {
    std::string line(reinterpret_cast<const char *>(data), size);
    espos_n2k::CanMessage msg;
    std::memset(&msg, 0xAA, sizeof(msg));  // poison: nothing read unless set

    if (espos_n2k::candump_decode(line.c_str(), &msg)) {
      // Claimed success, so the frame must be usable. dlc bounds matter:
      // it indexes data[8] on the way to the bus, and a decoder that
      // believes a line saying "64 bytes" writes past it.
      if (msg.frame.dlc > 8) {
        abort();
      }
      // Re-encoding what we just decoded must fit and must itself decode:
      // the gateway does exactly this round trip, bus -> client -> bus.
      char out[128];
      int n = espos_n2k::candump_encode(msg, "can0", out, sizeof(out));
      if (n > 0) {
        if (static_cast<size_t>(n) >= sizeof(out)) {
          abort();  // encode overran the buffer it was given
        }
        espos_n2k::CanMessage again;
        std::memset(&again, 0, sizeof(again));
        if (espos_n2k::candump_decode(out, &again)) {
          if (again.frame.id != msg.frame.id || again.frame.dlc != msg.frame.dlc) {
            abort();  // a frame that does not survive its own encoding
          }
        }
      }
    }
  }

  // ── resync: the property, at every possible cut ─────────────────────────
  //
  // The buffer is whatever the fuzzer produced, which is the point: real
  // buffers hold whole lines, and the function must not fall apart when they
  // do not. Whatever it keeps must start a line, never rewind, and never
  // point past the end.
  if (size > 0) {
    const char *buf = reinterpret_cast<const char *>(data);
    for (size_t sent = 0; sent <= size; sent++) {
      size_t keep = espos_n2k::candump_resync_offset(buf, size, sent);
      if (keep < sent || keep > size) {
        abort();  // rewound, or ran off the end
      }
      if (keep > 0 && keep < size && buf[keep - 1] != '\n') {
        abort();  // resumed in the middle of a line
      }
    }
  }

  return 0;
}
