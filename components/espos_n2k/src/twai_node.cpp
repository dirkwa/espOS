/* SPDX-License-Identifier: LicenseRef-Source-Available-No-Redistribution */
#include "twai_node.h"

#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"

namespace espos_n2k {
namespace detail {

namespace {
constexpr const char* kTag = "twai_node";

/// What the ISR hands the task. A classic CAN frame is small enough to copy
/// by value into a queue, which is what keeps the ISR short.
struct RxItem {
  CanMessage msg;
};
}  // namespace

TwaiNode& TwaiNode::instance() {
  static TwaiNode node;
  return node;
}

esp_err_t TwaiNode::acquire(const TwaiNodeConfig& config) {
  if (!lock_) {
    lock_ = xSemaphoreCreateMutex();
    if (!lock_) return ESP_ERR_NO_MEM;
  }
  xSemaphoreTake(lock_, portMAX_DELAY);

  if (node_) {
    // Already up. Joining with different pins would mean one of the two
    // callers is talking to a bus it did not configure; say so rather than
    // pretend.
    if (config.tx_pin != GPIO_NUM_NC &&
        (config.tx_pin != config_.tx_pin || config.rx_pin != config_.rx_pin ||
         config.bitrate != config_.bitrate)) {
      ESP_LOGW(kTag, "bus already up on TX=%d RX=%d %ukbps — ignoring the new config",
               (int)config_.tx_pin, (int)config_.rx_pin, (unsigned)(config_.bitrate / 1000));
    }
    refs_.fetch_add(1);
    xSemaphoreGive(lock_);
    return ESP_OK;
  }

  // Say so rather than binding pin -1 and reporting success: an application
  // that forgot to set the pins gets one clear line instead of a silent bus
  // that never receives anything.
  if (config.tx_pin == GPIO_NUM_NC || config.rx_pin == GPIO_NUM_NC) {
    ESP_LOGE(kTag, "tx_pin/rx_pin not set — TWAI not started");
    xSemaphoreGive(lock_);
    return ESP_ERR_INVALID_ARG;
  }

  config_ = config;

  rx_queue_ = xQueueCreate(config.rx_queue_depth, sizeof(RxItem));
  if (!rx_queue_) {
    xSemaphoreGive(lock_);
    return ESP_ERR_NO_MEM;
  }

  twai_onchip_node_config_t node_cfg = {};
  node_cfg.io_cfg.tx = config.tx_pin;
  node_cfg.io_cfg.rx = config.rx_pin;
  node_cfg.io_cfg.quanta_clk_out = GPIO_NUM_NC;
  node_cfg.io_cfg.bus_off_indicator = GPIO_NUM_NC;
  node_cfg.bit_timing.bitrate = config.bitrate;
  node_cfg.tx_queue_depth = config.tx_queue_depth;
  // The peripheral timestamps received frames when this is non-zero, but the
  // rest of espOS measures in esp_timer microseconds since boot and the two
  // are not the same clock. Left disabled; the task stamps on arrival, as the
  // old twai_receive() loop did.
  node_cfg.timestamp_resolution_hz = 0;

  esp_err_t err = twai_new_node_onchip(&node_cfg, &node_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "twai_new_node_onchip failed: %s", esp_err_to_name(err));
    teardown();
    xSemaphoreGive(lock_);
    return err;
  }

  const twai_event_callbacks_t cbs = {
      .on_tx_done = nullptr,
      .on_rx_done = &TwaiNode::on_rx_done,
      .on_state_change = &TwaiNode::on_state_change,
      .on_error = nullptr,
  };
  err = twai_node_register_event_callbacks(node_, &cbs, this);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "twai_node_register_event_callbacks failed: %s", esp_err_to_name(err));
    teardown();
    xSemaphoreGive(lock_);
    return err;
  }

  task_running_.store(true);
  if (xTaskCreate(&TwaiNode::rx_task, "twai_rx", 4096, this, 5, &task_) != pdPASS) {
    task_running_.store(false);
    ESP_LOGE(kTag, "could not start the twai task");
    teardown();
    xSemaphoreGive(lock_);
    return ESP_ERR_NO_MEM;
  }

  err = twai_node_enable(node_);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "twai_node_enable failed: %s", esp_err_to_name(err));
    // Let the task go before deleting the node it holds a pointer to: it
    // wakes at least every 100 ms and may be about to call into the driver.
    task_running_.store(false);
    for (int i = 0; i < 20 && task_; i++) vTaskDelay(pdMS_TO_TICKS(20));
    teardown();
    xSemaphoreGive(lock_);
    return err;
  }

  refs_.store(1);
  ESP_LOGI(kTag, "TWAI started: TX=%d RX=%d %ukbps", (int)config.tx_pin,
           (int)config.rx_pin, (unsigned)(config.bitrate / 1000));
  xSemaphoreGive(lock_);
  return ESP_OK;
}

void TwaiNode::release() {
  if (!lock_) return;
  xSemaphoreTake(lock_, portMAX_DELAY);
  if (refs_.load() <= 0) {
    xSemaphoreGive(lock_);
    return;
  }
  if (refs_.fetch_sub(1) != 1) {   // somebody else is still using the bus
    xSemaphoreGive(lock_);
    return;
  }

  if (node_) twai_node_disable(node_);
  task_running_.store(false);
  // The task wakes at least every 100 ms on its queue read and then exits.
  for (int i = 0; i < 20 && task_; i++) vTaskDelay(pdMS_TO_TICKS(20));
  teardown();
  ESP_LOGI(kTag, "TWAI stopped");
  xSemaphoreGive(lock_);
}

/// Caller holds lock_ (or is on the failure path of acquire()).
void TwaiNode::teardown() {
  if (node_) {
    twai_node_delete(node_);
    node_ = nullptr;
  }
  if (rx_queue_) {
    vQueueDelete(rx_queue_);
    rx_queue_ = nullptr;
  }
  refs_.store(0);
}

void TwaiNode::set_sink(FrameSink sink, void* ctx) {
  sink_ctx_.store(ctx);
  sink_.store(sink);
}

esp_err_t TwaiNode::transmit(const CanFrame& frame, int timeout_ms) {
  if (!node_) return ESP_ERR_INVALID_STATE;
  if (frame.dlc > kCanMaxData) return ESP_ERR_INVALID_ARG;

  twai_frame_t tx = {};
  tx.header.id = frame.id;
  tx.header.ide = frame.extended;
  tx.header.rtr = frame.remote;
  tx.header.dlc = frame.dlc;
  // The driver reads the payload from the buffer we point at; the frame is
  // copied into the driver's queue before transmit returns.
  tx.buffer = const_cast<uint8_t*>(frame.data);
  tx.buffer_len = frame.dlc;

  return twai_node_transmit(node_, &tx, timeout_ms);
}

/* ---------------------------------------------------------------- ISR side */

bool TwaiNode::on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t* edata, void* ctx) {
  (void)edata;
  auto* self = static_cast<TwaiNode*>(ctx);

  // twai_node_receive_from_isr() is only callable here, and only with a
  // buffer of our own to copy the payload into.
  uint8_t data[kCanMaxData];
  twai_frame_t rx = {};
  rx.buffer = data;
  rx.buffer_len = sizeof(data);
  if (twai_node_receive_from_isr(node, &rx) != ESP_OK) return false;

  RxItem item;
  item.msg.frame.id = rx.header.id;
  item.msg.frame.extended = rx.header.ide;
  item.msg.frame.remote = rx.header.rtr;
  item.msg.frame.dlc = rx.header.dlc > kCanMaxData ? kCanMaxData : (uint8_t)rx.header.dlc;
  memcpy(item.msg.frame.data, data, item.msg.frame.dlc);
  item.msg.timestamp_us = esp_timer_get_time();

  BaseType_t woken = pdFALSE;
  // A full queue drops the frame, which is what the old driver's rx_queue_len
  // did too. Not logged: this runs in an ISR, and a bus that outruns the
  // consumer would spend all its time logging.
  xQueueSendFromISR(self->rx_queue_, &item, &woken);
  return woken == pdTRUE;
}

bool TwaiNode::on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t* edata, void* ctx) {
  (void)node;
  auto* self = static_cast<TwaiNode*>(ctx);
  if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
    self->bus_off_count_.fetch_add(1, std::memory_order_relaxed);
    // Recovery is a task's job: twai_node_recover() is not ISR-safe.
    self->recover_pending_.store(true);
  }
  return false;
}

/* --------------------------------------------------------------- task side */

void TwaiNode::rx_task(void* arg) {
  auto* self = static_cast<TwaiNode*>(arg);

  while (self->task_running_.load()) {
    if (self->recover_pending_.exchange(false)) {
      ESP_LOGW(kTag, "bus-off — initiating recovery");
      esp_err_t err = twai_node_recover(self->node_);
      if (err != ESP_OK) {
        ESP_LOGE(kTag, "twai_node_recover failed: %s", esp_err_to_name(err));
      }
    }

    RxItem item;
    if (xQueueReceive(self->rx_queue_, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
      continue;
    }
    FrameSink sink = self->sink_.load();
    if (sink) sink(self->sink_ctx_.load(), item.msg);
  }

  self->task_ = nullptr;
  vTaskDelete(nullptr);
}

}  // namespace detail
}  // namespace espos_n2k
