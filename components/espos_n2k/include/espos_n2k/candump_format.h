/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#ifndef COCKPIT_N2K_CANDUMP_FORMAT_H_
#define COCKPIT_N2K_CANDUMP_FORMAT_H_

#include "espos_n2k/twai_message.h"

namespace espos_n2k {

/// Encode a TwaiMessage to candump ASCII format:
///   (1234567890.123456) vcan0 09F10203#FF00FF00FF00FF00\n
/// Returns number of bytes written (excluding null terminator),
/// or -1 if buf is too small.
int candump_encode(const TwaiMessage& msg, const char* iface,
                   char* buf, size_t buf_len);

/// Decode a candump ASCII line into a TwaiMessage.
/// Returns true on success, false on parse error.
bool candump_decode(const char* line, TwaiMessage* out);

}  // namespace espos_n2k

#endif  // COCKPIT_N2K_CANDUMP_FORMAT_H_
