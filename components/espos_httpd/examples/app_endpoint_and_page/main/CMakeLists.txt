# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_httpd for espos_httpd_register() and the SSE publisher. espos_sk and
# espos_ota are not called here; they are named so the device is a complete
# espOS device — a SignalK client with updates over the air — and not only a
# web server.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_httpd espos_sk espos_ota)
