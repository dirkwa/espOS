# SPDX-FileCopyrightText: 2026 Dirk Wahrheit
# SPDX-License-Identifier: Apache-2.0
#
# espos_config for the getters and the change feed, espos_sk to show the
# settings doing something, espos_ota because a device on a boat wants
# updates over the air.
idf_component_register(SRCS main.c PRIV_REQUIRES espos_core espos_config espos_sk espos_ota)

# One JSON file is the whole of a setting. From it the build generates the
# key constants main.c uses (espos_cfg_keys.h), the tables the store
# validates against, and the schema the web UI renders its Config form from;
# GET/PUT /api/v1/config follow. After idf_component_register, always.
espos_config_add_descriptor(config/app.json)
