// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// espos::flow — nodes and edges: the typed graph over espos_flow's C loop.
//
// This is SensESP's producer/consumer model with the four things that hurt in
// practice taken out:
//
//   * No `new` in user code and no ownership trap. A node is a member of your
//     struct, or lives in the Graph's intrusive list via make<T>(). SensESP's
//     ConfigItem(T*) took a pointer whose lifetime nobody owned (#893); here
//     there is nothing to hand over.
//   * No std::function and no shared_ptr per edge. An edge is a function
//     pointer, a sink pointer and a next pointer, taken from a static pool —
//     twelve bytes, no allocation, and it cannot fail at runtime because it
//     fails at wiring time instead.
//   * No per-type duplication of the bookkeeping. Everything that does not
//     depend on T (id, title, list link, wiring) lives in NodeBase, which is
//     compiled once; a new value type duplicates emit() and set() and nothing
//     else (SensESP #339, where the templates were most of the flash).
//   * One threading rule instead of five. Everything runs on the flow task.
//     See espos_flow.h; CONFIG_ESPOS_FLOW_CHECK_TASK enforces it.
//
// Implicit numeric conversion happens inside the edge trampoline, so wiring a
// producer of float into a consumer of int32_t needs no adapter node — the
// trampoline is instantiated for the (From, To) pair and does the cast.
//
// Compiles with -fno-exceptions -fno-rtti. No iostreams, no std::format, no
// allocation after start-up.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <utility>

#include "espos_flow.h"

namespace espos::flow {

// A node id is short and stable: it is what a log line, the config UI and a
// wiring error name. 12 characters plus NUL, stored inline — no allocation and
// no dangling pointer to a caller's temporary.
inline constexpr std::size_t kIdMax = 12;

// ─────────────────────────────────────────────────────────────────── edges
//
// The trampoline is `void (*)(void* sink, const void* value)`: type-erased so
// the pool is one array rather than one per T, and re-typed by the template
// that installed it. Nothing else may write these.
struct Edge {
  void (*fn)(void* sink, const void* value);
  void* sink;
  Edge* next;
};

// Static pool. Exhaustion is loud (a log line naming the producer, then an
// abort) rather than a dropped connection: a graph missing an edge looks
// exactly like a sensor that stopped working, and finding that months later
// costs more than failing at boot.
Edge* edge_alloc();
std::size_t edge_used();
std::size_t edge_capacity();
[[noreturn]] void edge_exhausted(const char* node_id);

// Called by emit() under CONFIG_ESPOS_FLOW_CHECK_TASK. Out of line so the
// check is one call, not an inlined log statement per instantiation.
void assert_on_loop_task(const char* node_id);

#if CONFIG_ESPOS_FLOW_CHECK_TASK
#define ESPOS_FLOW_ASSERT_TASK(id) ::espos::flow::assert_on_loop_task(id)
#else
#define ESPOS_FLOW_ASSERT_TASK(id) ((void)0)
#endif

// ──────────────────────────────────────────────────────────────── NodeBase
//
// Everything about a node that does not depend on its value type. Compiled
// once for the whole firmware.
class NodeBase {
 public:
  explicit NodeBase(const char* id = "") { set_id(id); }

  NodeBase(const NodeBase&) = delete;
  NodeBase& operator=(const NodeBase&) = delete;

  // Not virtual: a node is never deleted through a base pointer. The graph
  // owns its nodes for the life of the firmware, which is the only lifetime
  // an embedded data-flow graph has ever needed, and a vtable per node type
  // is flash spent on a destructor that never runs.
  ~NodeBase() = default;

  const char* id() const { return id_; }
  const char* title() const { return title_; }

  // Chainable, so a node can be described where it is created:
  //   auto& t = g.make<Poll<float>>("temp", 1000, read).with_title("Coolant");
  NodeBase& with_title(const char* t) {
    title_ = t ? t : "";
    return *this;
  }

  // Intrusive list link, walked by Graph. Public because Graph is not a
  // friend of every node type and an accessor pair would be the same thing
  // with more code.
  NodeBase* next_node = nullptr;

 protected:
  void set_id(const char* id) {
    if (!id) id = "";
    std::size_t n = 0;
    while (n < kIdMax && id[n] != '\0') n++;
    std::memcpy(id_, id, n);
    id_[n] = '\0';
  }

 private:
  char id_[kIdMax + 1] = {};
  const char* title_ = "";
};

// ──────────────────────────────────────────────────────────────── Consumer
//
// Anything that can be handed a T. `set()` runs on the flow task.
template <typename T>
class Consumer {
 public:
  virtual void set(const T& value) = 0;

 protected:
  // Protected and non-virtual: consumers are never deleted polymorphically
  // (see NodeBase), and this keeps the class free of a deleting destructor
  // in every vtable.
  ~Consumer() = default;
};

// ──────────────────────────────────────────────────────────────── Producer
//
// Anything that emits a T. Holds the head of its edge list, its last value,
// and whether it has ever produced one — the distinction between "0.0" and
// "nothing yet" that a Join's age rule and an SK publish both need.
template <typename T>
class Producer {
 public:
  // The last value emitted. Undefined-but-safe (a value-initialised T)
  // before the first emit; check has_value() when that matters.
  const T& get() const { return value_; }
  bool has_value() const { return has_value_; }

  // When it was emitted, on espos_flow_now_ms()'s modular clock. 0 before
  // the first emit.
  uint32_t emitted_at_ms() const { return at_ms_; }

  // Push a value to every connected consumer, in connection order.
  //
  // Runs on the flow task; under CONFIG_ESPOS_FLOW_CHECK_TASK a call from
  // anywhere else aborts naming the node. Re-entrant emits are fine (a
  // consumer may emit onward, which is what a chain IS); a cycle is the
  // caller's problem and shows up as a stack overflow, not as a hang.
  void emit(const T& value) {
    value_ = value;
    has_value_ = true;
    at_ms_ = espos_flow_now_ms();
    for (Edge* e = edges_; e; e = e->next) e->fn(e->sink, &value_);
  }

  // Wire this producer to a consumer of U. T must be convertible to U;
  // the conversion happens in the trampoline, so float → int32_t needs no
  // adapter node.
  //
  // Returns the SINK, so chains read left to right in the order the data
  // moves: a.connect_to(b).connect_to(c). SensESP returns the same thing
  // for the same reason.
  template <typename Sink>
  Sink& connect_to(Sink& sink) {
    using U = typename Sink::consumes_type;
    static_assert(std::is_convertible_v<T, U>,
                  "connect_to: this producer's value type does not convert to "
                  "the consumer's");

    Edge* e = edge_alloc();
    if (!e) edge_exhausted(node_id_for_error());

    e->sink = static_cast<Consumer<U>*>(&sink);
    // One trampoline per (T, U) pair, not per node: the compiler folds
    // every float→float edge in the firmware onto the same function.
    e->fn = [](void* s, const void* v) {
      static_cast<Consumer<U>*>(s)->set(
          static_cast<U>(*static_cast<const T*>(v)));
    };
    e->next = nullptr;

    // Append rather than prepend: consumers are served in the order they
    // were connected, which is the order the wiring reads in. A reversed
    // fan-out is the kind of thing that is only noticed when two sinks
    // disagree about which value was newest.
    if (!edges_) {
      edges_ = e;
    } else {
      Edge* tail = edges_;
      while (tail->next) tail = tail->next;
      tail->next = e;
    }
    return sink;
  }

  // Sugar for the same thing: a >> b >> c. Returns the sink like
  // connect_to(), so the chain composes.
  template <typename Sink>
  Sink& operator>>(Sink& sink) {
    return connect_to(sink);
  }

  // How many consumers this producer feeds. For tests and diagnostics.
  std::size_t edge_count() const {
    std::size_t n = 0;
    for (const Edge* e = edges_; e; e = e->next) n++;
    return n;
  }

  using produces_type = T;

 protected:
  ~Producer() = default;

  // Nodes that are also NodeBase override this so a wiring failure names
  // them. A bare Producer has no id to give.
  virtual const char* node_id_for_error() const { return "?"; }

 private:
  Edge* edges_ = nullptr;
  T value_{};
  bool has_value_ = false;
  uint32_t at_ms_ = 0;
};

// A node that is both: the shape of every transform.
template <typename In, typename Out>
class Transform : public NodeBase, public Consumer<In>, public Producer<Out> {
 public:
  explicit Transform(const char* id) : NodeBase(id) {}
  using consumes_type = In;

 protected:
  ~Transform() = default;
  const char* node_id_for_error() const override { return id(); }
};

// In == Out. Most transforms are this: a calibration, a filter, a limiter.
template <typename T>
using Symmetric = Transform<T, T>;

// ─────────────────────────────────────────────────────────────────── Value
//
// SensESP's ObservableValue and ValueProducer collapsed into one type: a
// producer whose value anything may set. Setting emits.
//
// This is the node to reach for when something outside the graph — a config
// change, a REST handler, another component — has a number the graph should
// see, and the setting already happens on the flow task. When it does not,
// wrap it in a Mailbox.
template <typename T>
class Value : public NodeBase, public Producer<T>, public Consumer<T> {
 public:
  explicit Value(const char* id, const T& initial = T{}) : NodeBase(id) {
    if constexpr (!std::is_same_v<T, void>) initial_ = initial;
  }

  using consumes_type = T;

  // Set and emit. Also the Consumer entry point, so a Value can sit in the
  // middle of a chain as a named, inspectable tap.
  void set(const T& v) override {
    ESPOS_FLOW_ASSERT_TASK(id());
    this->emit(v);
  }

  // The value the node was constructed with, before anything set it. Kept
  // so a node can be reset to it without the caller remembering.
  const T& initial() const { return initial_; }

 protected:
  const char* node_id_for_error() const override { return id(); }

 private:
  T initial_{};
};

}  // namespace espos::flow
