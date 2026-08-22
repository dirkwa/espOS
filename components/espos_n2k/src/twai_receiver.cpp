/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "espos_n2k/twai_receiver.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "twai_node.h"

namespace espos_n2k {

namespace {
constexpr const char* kTag = "twai_rx";
}

TwaiReceiver::TwaiReceiver(const TwaiReceiverConfig& config)
    : config_(config) {}

TwaiReceiver::~TwaiReceiver() { stop(); }

void TwaiReceiver::start() {
  if (running_.exchange(true)) return;

  detail::TwaiNodeConfig cfg;
  cfg.tx_pin = config_.tx_pin;
  cfg.rx_pin = config_.rx_pin;
  cfg.bitrate = config_.bitrate;
  cfg.rx_queue_depth = config_.rx_queue_depth;

  // The node logs why on every failure path (unset pins, no free controller);
  // repeating it here would only say it twice.
  if (detail::TwaiNode::instance().acquire(cfg) != ESP_OK) {
    running_.store(false);
    return;
  }
  detail::TwaiNode::instance().set_sink(&TwaiReceiver::sink, this);
}

void TwaiReceiver::stop() {
  if (!running_.exchange(false)) return;
  // Unhook first: releasing may keep the bus up for a transmitter that is
  // still running, and a frame arriving after this object is gone would call
  // into a destroyed std::function.
  detail::TwaiNode::instance().set_sink(nullptr, nullptr);
  detail::TwaiNode::instance().release();
}

uint32_t TwaiReceiver::bus_off_count() const {
  return detail::TwaiNode::instance().bus_off_count();
}

/// Runs on the node's task, one frame at a time — the same contract the
/// old twai_receive() loop offered, so callbacks written against it are
/// unaffected by the driver change.
void TwaiReceiver::sink(void* ctx, const CanMessage& msg) {
  auto* self = static_cast<TwaiReceiver*>(ctx);
  self->last_rx_us_.store(msg.timestamp_us, std::memory_order_relaxed);
  if (self->on_frame_) self->on_frame_(msg);
}

}  // namespace espos_n2k
