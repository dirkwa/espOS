// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow::Graph — who owns the nodes.
//
// Two ownership styles, both without `new` in user code and both without
// SensESP's ConfigItem(T*) trap (#893), where a raw pointer was handed to a
// registry that neither owned it nor outlived it:
//
//   1. `g.make<Poll<float>>("temp", 1000, read)` — the Graph holds the node
//      for the life of the firmware and hands back a reference. This is the
//      wiring-function style, and the one the tutorials use.
//
//   2. A node as a member of your own struct, wired in its constructor. The
//      struct owns it; the Graph never sees it. This is the driver style: a
//      `struct Bme280 { Poll<float> temp; ... }` that registers itself.
//
// Both are safe because nothing in the graph ever deletes a node, and an edge
// is a plain pointer into memory that outlives the loop.
//
// Storage for make<T>() is a static arena, not the heap: the size of a graph
// is decided at build time like every other espOS table, and a firmware that
// makes no nodes pays no arena — the arena is only linked when make<T>() is
// instantiated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#include "espos_flow.h"
#include "espos_flow/node.hpp"

namespace espos::flow {

// Bytes handed out by Graph::make<T>(). Not a Kconfig option because it is a
// linker-visible arena whose size only matters if make<T>() is used at all;
// a firmware that needs more overrides it at the compiler command line.
#ifndef ESPOS_FLOW_ARENA_BYTES
#define ESPOS_FLOW_ARENA_BYTES 2048
#endif

// Raw storage for make<T>(). Out of line so it is only linked into a firmware
// that actually calls make<T>().
void* graph_arena_alloc(std::size_t bytes, std::size_t align);
std::size_t graph_arena_used();
std::size_t graph_arena_capacity();
[[noreturn]] void graph_arena_exhausted(std::size_t wanted);

class Graph {
 public:
  Graph() = default;
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;

  // Construct a node in the graph's arena and keep it forever. The
  // reference is stable: nothing moves or frees it.
  //
  //   auto& poll = g.make<Poll<float>>("depth", 1000, read_depth);
  //   auto& lin  = g.make<Linear<float>>("cal", 1.7f, -0.16f);
  //   poll >> lin >> g.make<Sink<float>>("out", publish);
  template <typename T, typename... Args>
  T& make(Args&&... args) {
    void* mem = graph_arena_alloc(sizeof(T), alignof(T));
    if (!mem) graph_arena_exhausted(sizeof(T));
    T* node = new (mem) T(std::forward<Args>(args)...);
    adopt(*node);
    return *node;
  }

  // Put an externally owned node on the graph's list, so it appears in
  // for_each() and in a future config page. The node must outlive the
  // graph; a member of a struct that lives forever does.
  void adopt(NodeBase& n) {
    n.next_node = nullptr;
    if (!head_) {
      head_ = &n;
    } else {
      tail_->next_node = &n;
    }
    tail_ = &n;
    count_++;
  }

  // Start the flow loop. Nothing runs before this: a node constructed and
  // wired is inert until the loop exists, which is what lets a whole graph
  // be built in static initialisers before app_main() decides to run it.
  esp_err_t start() { return espos_flow_start(); }

  std::size_t size() const { return count_; }
  NodeBase* first() const { return head_; }

  // Walk every adopted node. `fn` is any callable taking NodeBase& — a
  // template rather than std::function, which is not allowed here.
  template <typename Fn>
  void for_each(Fn&& fn) const {
    for (NodeBase* n = head_; n; n = n->next_node) fn(*n);
  }

 private:
  NodeBase* head_ = nullptr;
  NodeBase* tail_ = nullptr;
  std::size_t count_ = 0;
};

}  // namespace espos::flow
