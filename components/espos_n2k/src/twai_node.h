/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#ifndef ESPOS_N2K_SRC_TWAI_NODE_H_
#define ESPOS_N2K_SRC_TWAI_NODE_H_

/// The one TWAI node, shared by the receiver and the transmitter.
///
/// IDF 6's esp_twai API allocates a *node* and hands back a handle, where the
/// old driver/twai.h API installed a process-wide singleton that any caller
/// could reach through twai_receive()/twai_transmit(). TwaiReceiver and
/// TwaiTransmitter were written against that singleton and are separate
/// objects with separate lifetimes, so somebody has to own the handle now.
/// This is that somebody: a reference-counted holder that behaves the way the
/// old global driver did — the first user to start configures the bus, the
/// last one to stop tears it down.
///
/// Internal to the component. Nothing here is part of the public API.

#include <atomic>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "espos_n2k/can_frame.h"

namespace espos_n2k {
namespace detail {

struct TwaiNodeConfig {
  gpio_num_t tx_pin = GPIO_NUM_NC;
  gpio_num_t rx_pin = GPIO_NUM_NC;
  uint32_t bitrate = 250000;
  size_t rx_queue_depth = 64;
  size_t tx_queue_depth = 32;
};

class TwaiNode {
 public:
  using FrameSink = void (*)(void* ctx, const CanMessage& msg);

  static TwaiNode& instance();

  /// Start (or join) the bus. The first caller's config wins; a later caller
  /// with a different one gets a warning, not a silently reconfigured bus.
  /// Reference-counted against release().
  esp_err_t acquire(const TwaiNodeConfig& config);
  void release();

  bool running() const { return refs_.load() > 0; }

  /// Frames are delivered on the node's own task, never from the ISR.
  void set_sink(FrameSink sink, void* ctx);

  esp_err_t transmit(const CanFrame& frame, int timeout_ms);

  uint32_t bus_off_count() const { return bus_off_count_.load(); }

 private:
  TwaiNode() = default;

  static bool on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t* edata, void* ctx);
  static bool on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t* edata, void* ctx);
  static void rx_task(void* arg);

  void teardown();

  twai_node_handle_t node_ = nullptr;
  TwaiNodeConfig config_;
  std::atomic<int> refs_{0};
  std::atomic<bool> task_running_{false};
  /// Set from the state-change ISR, acted on by the task: twai_node_recover()
  /// is not safe to call from an ISR.
  std::atomic<bool> recover_pending_{false};
  std::atomic<uint32_t> bus_off_count_{0};

  QueueHandle_t rx_queue_ = nullptr;
  TaskHandle_t task_ = nullptr;
  SemaphoreHandle_t lock_ = nullptr;

  /// Read by the task, written by set_sink(); a pointer pair small enough
  /// that a torn read is not possible on any target espOS builds for.
  std::atomic<FrameSink> sink_{nullptr};
  std::atomic<void*> sink_ctx_{nullptr};
};

}  // namespace detail
}  // namespace espos_n2k

#endif  // ESPOS_N2K_SRC_TWAI_NODE_H_
