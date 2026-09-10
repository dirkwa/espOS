// SPDX-FileCopyrightText: 2026 Dirk Wahrheit
// SPDX-License-Identifier: Apache-2.0
//
// GET /api/v1/flow — what the graph is, and what the loop is doing.
//
// SensESP put this on a status page assembled from StatusPageItem objects a
// firmware registered by hand. The graph already knows its own shape, so this
// asks it instead: nothing to register, nothing to keep in step with the
// wiring, and a node added to the graph appears here without being told to.
//
// It answers two different questions, which is why both halves are here:
//
//   "is my wiring what I think it is"   — the node list, in adoption order
//   "is the loop keeping up"            — posts, drops, queue peak, edges
//
// The second is the one that matters in service. `dropped` climbing means
// something is posting faster than the loop consumes, and `edges_used` near
// CONFIG_ESPOS_FLOW_MAX_EDGES means the next connect_to() will fail — both
// are invisible from outside and neither raises an alarm.
#include <cstdio>

#include "espos_flow.h"
#include "espos_flow/graph.hpp"

#if __has_include("espos_httpd.h")
#include "cJSON.h"
#include "espos_httpd.h"

namespace {

const espos::flow::Graph* s_graph = nullptr;

esp_err_t get_flow(httpd_req_t* req) {
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    return espos_httpd_send_error(req, "500 Internal Server Error", "no_memory",
                                  "out of memory");
  }

  cJSON_AddBoolToObject(root, "running", espos_flow_is_running());

  // The runtime's own counters, which exist whether or not a Graph was
  // registered: a firmware can use the C timer/mailbox half alone.
  espos_flow_stats_t st = {};
  espos_flow_stats(&st);
  cJSON* s = cJSON_AddObjectToObject(root, "loop");
  if (s) {
    cJSON_AddNumberToObject(s, "posts", (double)st.posts);
    cJSON_AddNumberToObject(s, "dropped", (double)st.dropped);
    cJSON_AddNumberToObject(s, "timers_fired", (double)st.timers_fired);
    cJSON_AddNumberToObject(s, "timers_live", (double)st.timers_live);
    cJSON_AddNumberToObject(s, "queue_peak", (double)st.queue_peak);
    cJSON_AddNumberToObject(s, "edges_used", (double)st.edges_used);
    cJSON_AddNumberToObject(s, "edges_max",
                            (double)CONFIG_ESPOS_FLOW_MAX_EDGES);
  }

  // The graph is optional: a firmware may drive the loop from C and never
  // build one. Null rather than an empty array, so a client can tell "no
  // graph" from "a graph with no nodes" -- the second is a wiring bug.
  if (!s_graph) {
    cJSON_AddNullToObject(root, "nodes");
  } else {
    cJSON* nodes = cJSON_AddArrayToObject(root, "nodes");
    if (nodes) {
      // Adoption order, which is the order make<>() was called -- the order
      // the firmware's own source reads in, not an internal one.
      s_graph->for_each([nodes](const espos::flow::NodeBase& n) {
        cJSON* o = cJSON_CreateObject();
        if (!o) return;
        cJSON_AddStringToObject(o, "id", n.id());
        const char* t = n.title();
        if (t && *t) cJSON_AddStringToObject(o, "title", t);
        cJSON_AddItemToArray(nodes, o);
      });
    }
  }

  char* json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json) {
    return espos_httpd_send_error(req, "500 Internal Server Error", "no_memory",
                                  "out of memory");
  }
  esp_err_t err = espos_httpd_send_json(req, nullptr, json);
  cJSON_free(json);
  return err;
}

}  // namespace

namespace espos::flow {

esp_err_t api_register(const Graph* graph) {
  s_graph = graph;
  static const httpd_uri_t uri = {
      .uri = "/api/v1/flow",
      .method = HTTP_GET,
      .handler = get_flow,
      .user_ctx = nullptr,
  };
  return espos_httpd_register(&uri);
}

}  // namespace espos::flow

#else  // no espos_httpd in this build

namespace espos::flow {

esp_err_t api_register(const Graph* graph) {
  (void)graph;
  return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace espos::flow

#endif
