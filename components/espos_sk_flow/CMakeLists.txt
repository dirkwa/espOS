# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_sk_flow: the Signal K nodes of the graph (docs/signalk.md).
#
# Header-only on purpose. Every node here is a thin adapter over an
# espos_sk call, and the C++ that adapts it is smaller than the function
# call it wraps; putting it in a .cpp would add a translation unit and a
# link dependency for a firmware that uses one node out of eight.
#
# Kept apart from espos_sensors so that neither implies the other: a display
# firmware wants the Signal K nodes and no sensors, an offline datalogger
# wants the sensors and no server. espos_flow is the only thing they share.
idf_component_register(
    INCLUDE_DIRS include
    REQUIRES espos_flow espos_sk espos_net espos_health)
