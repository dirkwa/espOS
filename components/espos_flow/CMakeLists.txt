# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_flow: the data-flow runtime — one task, one timer wheel, one mailbox,
# and the typed C++ graph over them (docs/flow.md).
#
# Two halves on purpose. src/ is C: the loop, the wheel, the mailbox, usable
# on its own by a firmware that only wants "call this every 500 ms, from a
# task I can post to". cpp/ is the C++ graph's out-of-line part — the edge
# pool, the node arena, the wrong-task assertion and the common template
# instantiations, which exist so that a new node type duplicates emit() and
# set() and nothing else.
#
# REQUIRES is deliberately tiny. espos_flow is the layer everything else will
# eventually be wired through, so it must not depend on what is being wired:
# no espos_sk, no espos_config, no espos_net. espos_health is optional --
# reporting a full mailbox is worth it, and a firmware built without it simply
# loses that one report (CONFIG_ESPOS_FLOW_HEALTH).
#
# espos_formulas is the one hard REQUIRES, and only because
# transforms/marine.hpp is written on top of it: every class in that header
# calls into it, so there is no reduced version to fall back to. It is
# header-only with no dependencies of its own, so this adds an include
# directory and links nothing.
# The monotonic clock is the one platform dependency, behind flow_port.h:
# esp_timer on a chip, CLOCK_MONOTONIC on the linux target, where IDF ships
# esp_timer's header but not esp_timer_get_time(). Same split as espos_time.
idf_build_get_property(target IDF_TARGET)
set(srcs src/espos_sched.c src/espos_flow.c cpp/flow_graph.cpp cpp/flow_instantiations.cpp cpp/api_flow.cpp)
if(target STREQUAL "linux")
    list(APPEND srcs src/port_sim.c)
    set(priv_reqs "")
else()
    list(APPEND srcs src/port_idf.c)
    set(priv_reqs esp_timer)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include
    REQUIRES espos_formulas
    PRIV_REQUIRES ${priv_reqs})

# Linked only when the firmware also builds espos_health; the source guards
# the call with CONFIG_ESPOS_FLOW_HEALTH, which a firmware without the
# component turns off.
idf_component_optional_requires(PRIVATE espos_health)

# GET /api/v1/flow is worth having and not worth requiring: a firmware with
# no web server is a legitimate one, and api_flow.cpp guards itself with
# __has_include so it compiles to a stub there. The optional dependency is
# what puts espos_httpd's headers on the include path when it IS in the
# build -- without it __has_include answers no even though the component is
# present.
idf_component_optional_requires(PRIVATE espos_httpd espressif__cjson)

# C11 for the runtime, C++ for the graph. IDF 6 compiles device code at
# -std=gnu++26 with exceptions and RTTI off; the graph is written to that
# (std::optional/span/string_view are fine, std::function and iostreams are
# not), so nothing here needs a per-target flag.

# Tell the source whether espos_health is actually in the build. The Kconfig
# option asks for the report; this says whether there is anything to report
# to, so a firmware that keeps the default and drops the component still
# compiles.
idf_build_get_property(comps BUILD_COMPONENTS)
if(espos_health IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPOS_FLOW_HAVE_HEALTH=1)
endif()

# The transform headers grow two optional capabilities, and unlike the health
# report above these are consumed in PUBLIC headers -- param.hpp binds a node
# parameter to a config namespace, timing.hpp stamps a wall-clock time -- so
# the definitions must be INTERFACE. A PRIVATE define would be visible while
# compiling flow's own sources and invisible to the firmware including the
# same header, which is exactly the kind of ODR split that links cleanly and
# then behaves differently in two translation units.
# espos_formulas is NOT optional, unlike the two above. transforms/marine.hpp
# includes it unconditionally and every class in that header is built on it, so
# there is nothing to fall back to: made optional, a firmware that did not name
# espos_formulas itself failed to compile the moment it included
# espos_flow/transforms.hpp. It costs nothing to require -- the component is
# header-only with no dependencies of its own beyond the C++ standard library,
# so REQUIREing it adds an include directory and links no code.
idf_component_optional_requires(PUBLIC espos_config espos_time)
if(espos_config IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} INTERFACE ESPOS_FLOW_HAVE_CONFIG=1)
endif()
if(espos_time IN_LIST comps)
    target_compile_definitions(${COMPONENT_LIB} INTERFACE ESPOS_FLOW_HAVE_TIME=1)
endif()
