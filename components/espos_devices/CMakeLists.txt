# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_devices: whole devices, not parts (docs/devices.md).
#
# Header-only, and deliberately so. A device class is a struct that owns four
# nodes and wires them in its constructor; there is nothing to put in a .c
# file, and a firmware that uses none of them pays nothing.
#
# REQUIRES is the union of what the devices compose from, because those types
# appear in their public headers -- a firmware that includes espos_devices is
# already using espos_flow's graph and espos_sk_flow's Output, and would have
# named them itself.
idf_component_register(
    INCLUDE_DIRS include
    REQUIRES espos_flow espos_sensors espos_sk_flow)
