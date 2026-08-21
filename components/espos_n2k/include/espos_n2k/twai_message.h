/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#ifndef COCKPIT_N2K_TWAI_MESSAGE_H_
#define COCKPIT_N2K_TWAI_MESSAGE_H_

#include "driver/twai.h"

namespace espos_n2k {

/// Thin wrapper around twai_message_t with a microsecond timestamp.
struct TwaiMessage {
  twai_message_t frame;
  int64_t timestamp_us;  // esp_timer_get_time()
};

}  // namespace espos_n2k

#endif  // COCKPIT_N2K_TWAI_MESSAGE_H_
