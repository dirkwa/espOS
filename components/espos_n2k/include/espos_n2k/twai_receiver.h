/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#ifndef COCKPIT_N2K_TWAI_RECEIVER_H_
#define COCKPIT_N2K_TWAI_RECEIVER_H_

#include <atomic>
#include <cstdint>

#include "driver/gpio.h"
#include "driver/twai.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <functional>
#include "espos_n2k/twai_message.h"

namespace espos_n2k {

struct TwaiReceiverConfig {
  gpio_num_t tx_pin = GPIO_NUM_22;
  gpio_num_t rx_pin = GPIO_NUM_21;
  uint32_t bitrate = 250000;  // NMEA 2000 standard
  size_t rx_queue_depth = 64;

  /// Factory preset for the Waveshare ESP32-P4-WIFI6-Touch-LCD-7B
  /// which has an on-board TJA1051T CAN transceiver on GPIO22/21.
  static TwaiReceiverConfig waveshare_touch_lcd_7b() {
    return {.tx_pin = GPIO_NUM_22, .rx_pin = GPIO_NUM_21};
  }
};

/// Reads CAN frames from the TWAI peripheral and emits them as
/// TwaiMessage values. Runs a dedicated FreeRTOS task for RX.
class TwaiReceiver {
 public:
  using FrameFn = std::function<void(const TwaiMessage&)>;
  explicit TwaiReceiver(const TwaiReceiverConfig& config = {});
  ~TwaiReceiver();

  void start();
  void stop();

  /// Called on the receiver task for every frame. Set before start().
  void set_on_frame(FrameFn fn) { on_frame_ = std::move(fn); }

  /// True if we have received at least one frame since boot.
  bool ever_received() const {
    return last_rx_us_.load(std::memory_order_relaxed) != 0;
  }

  /// Seconds since the last received frame. Returns INT64_MAX if no
  /// frame has ever been received. esp_timer_get_time() returns int64
  /// microseconds since boot — overflows in ~292,000 years, so no
  /// rollover concerns. (Was rx_count_ uint32 which overflowed at
  /// ~20 days of busy N2K traffic.)
  int64_t seconds_since_last_rx() const {
    int64_t last = last_rx_us_.load(std::memory_order_relaxed);
    if (last == 0) return INT64_MAX;
    return (esp_timer_get_time() - last) / 1000000;
  }

  uint32_t bus_off_count() const { return bus_off_count_; }

 private:
  static void rx_task(void* arg);

  TwaiReceiverConfig config_;
  FrameFn on_frame_;
  TaskHandle_t rx_task_ = nullptr;
  std::atomic<bool> running_{false};
  // Microseconds-since-boot of last RX frame; 0 = nothing received yet.
  std::atomic<int64_t> last_rx_us_{0};
  std::atomic<uint32_t> bus_off_count_{0};
};

}  // namespace espos_n2k

#endif  // COCKPIT_N2K_TWAI_RECEIVER_H_
