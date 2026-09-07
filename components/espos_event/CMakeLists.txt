# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# Leaf: the espOS event base on the default esp_event loop. It depends on no
# other espOS component so that any of them may post here without a cycle —
# espos_core requires the core components, and the core components post.
idf_component_register(
    SRCS src/espos_event.c
    INCLUDE_DIRS include
    REQUIRES esp_event)
