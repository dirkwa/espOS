# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_event for ESPOS_EVENT_SK_STREAM_CONNECTED, espos_sk for the publish
# and the REST lookup (espos_sk_http.h). espos_ota is not called here; it is
# named so phase two really is httpd → wifi → sk → ota on this device.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_event espos_sk espos_ota)
