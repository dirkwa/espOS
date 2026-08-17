# SPDX-License-Identifier: Apache-2.0
idf_component_register(
    SRCS test_main.c test_core.c test_json.c test_migrations.c test_b64.c test_nvs_backend.c
    INCLUDE_DIRS "."
    PRIV_REQUIRES espos_config unity nvs_flash esp_partition espressif__cjson esp_rom
    WHOLE_ARCHIVE)

espos_config_add_descriptor(config/t1.json)
espos_config_add_descriptor(config/mig.json)
espos_config_add_descriptor(config/sec.json)
espos_config_add_descriptor(config/big.json)
