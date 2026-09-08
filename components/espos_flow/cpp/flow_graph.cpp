// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// The out-of-line half of the C++ graph: the edge pool, the node arena, and
// the wrong-task assertion. Everything here is compiled once for the whole
// firmware, however many node types it instantiates — which is the point.
// SensESP #339 measured the templates as most of the flash; the cure is to
// keep everything that does not depend on T out of the templates.
#include <cstdio>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "espos_flow.h"
#include "espos_flow/graph.hpp"
#include "espos_flow/node.hpp"

namespace espos::flow {

static const char* TAG = "espos_flow";

// ────────────────────────────────────────────────────────────── edge pool

static Edge s_edges[CONFIG_ESPOS_FLOW_MAX_EDGES];
static std::size_t s_edges_used = 0;

Edge* edge_alloc() {
  // Wiring happens on one task during start-up, before the loop runs, so
  // there is nothing to synchronise with. A graph rewired at runtime from
  // two tasks at once is not a supported shape and would be a far bigger
  // problem than this counter.
  if (s_edges_used >= CONFIG_ESPOS_FLOW_MAX_EDGES) return nullptr;
  Edge* e = &s_edges[s_edges_used++];
  espos_flow_note_edges_used(static_cast<uint32_t>(s_edges_used));
  return e;
}

std::size_t edge_used() { return s_edges_used; }

std::size_t edge_capacity() { return CONFIG_ESPOS_FLOW_MAX_EDGES; }

void edge_exhausted(const char* node_id) {
  // Loud and fatal, at wiring time. A dropped edge would be a firmware that
  // boots, looks healthy, and quietly never publishes one of its sensors —
  // the failure that is found in a season rather than in a second.
  ESP_LOGE(TAG, "edge pool exhausted wiring node '%s': %d edges used of %d.",
           node_id ? node_id : "?", static_cast<int>(s_edges_used),
           CONFIG_ESPOS_FLOW_MAX_EDGES);
  ESP_LOGE(TAG,
           "raise CONFIG_ESPOS_FLOW_MAX_EDGES; one edge per connect_to().");
  abort();
}

// ─────────────────────────────────────────────────────────────── node arena

alignas(alignof(
    std::max_align_t)) static unsigned char s_arena[ESPOS_FLOW_ARENA_BYTES];
static std::size_t s_arena_used = 0;

void* graph_arena_alloc(std::size_t bytes, std::size_t align) {
  std::size_t base = (s_arena_used + align - 1) & ~(align - 1);
  if (base + bytes > sizeof(s_arena)) return nullptr;
  s_arena_used = base + bytes;
  return &s_arena[base];
}

std::size_t graph_arena_used() { return s_arena_used; }

std::size_t graph_arena_capacity() { return sizeof(s_arena); }

void graph_arena_exhausted(std::size_t wanted) {
  ESP_LOGE(TAG, "graph arena exhausted: %d more bytes wanted, %d of %d used.",
           static_cast<int>(wanted), static_cast<int>(s_arena_used),
           static_cast<int>(sizeof(s_arena)));
  ESP_LOGE(TAG,
           "raise ESPOS_FLOW_ARENA_BYTES, or hold the node as a member of your "
           "own struct.");
  abort();
}

// ───────────────────────────────────────────────────── wrong-task assertion

void assert_on_loop_task(const char* node_id) {
  if (espos_flow_on_loop_task()) return;

  // Naming both the node and the offending task is the whole value here: a
  // value corrupted by a cross-task emit surfaces days later as an
  // impossible reading, and by then nothing points at the wiring.
  const char* task = pcTaskGetName(nullptr);
  ESP_LOGE(TAG, "node '%s': emit() from task '%s', not the flow loop.",
           node_id ? node_id : "?", task ? task : "?");
  ESP_LOGE(TAG,
           "everything in a graph runs on the flow task; values from elsewhere "
           "come in through a Mailbox.");
  abort();
}

}  // namespace espos::flow
