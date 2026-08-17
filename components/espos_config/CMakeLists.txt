# SPDX-License-Identifier: Apache-2.0
set(ESPOS_CONFIG_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/gen")
set(ESPOS_CONFIG_GEN_C "${ESPOS_CONFIG_GEN_DIR}/espos_config_gen.c")
set(ESPOS_CONFIG_GEN_H "${ESPOS_CONFIG_GEN_DIR}/include/espos_cfg_keys.h")
set(ESPOS_CONFIG_GEN_SCHEMA "${ESPOS_CONFIG_GEN_DIR}/espos_config_schema.json")
set(ESPOS_CONFIG_GEN_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../../tools/espos_gen_config.py")
get_filename_component(ESPOS_CONFIG_GEN_SCRIPT "${ESPOS_CONFIG_GEN_SCRIPT}" ABSOLUTE)
# The include dir must exist at configure time for idf_component_register.
file(MAKE_DIRECTORY "${ESPOS_CONFIG_GEN_DIR}/include")

set(srcs
    src/espos_config.c
    src/espos_config_json.c
    src/backend_mem.c
    src/b64.c
    "${ESPOS_CONFIG_GEN_C}")

# The NVS backend needs nvs_flash; it builds for every chip target and (with
# the file-backed flash emulation) for the linux host too.
list(APPEND srcs src/backend_nvs.c)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS include "${ESPOS_CONFIG_GEN_DIR}/include"
    REQUIRES nvs_flash
    PRIV_REQUIRES espressif__cjson)

# Generate schema + tables from every descriptor registered via
# espos_config_add_descriptor(). The list is read at generate time.
idf_build_get_property(python PYTHON)
idf_build_get_property(descriptors ESPOS_CONFIG_DESCRIPTORS GENERATOR_EXPRESSION)
add_custom_command(
    OUTPUT "${ESPOS_CONFIG_GEN_C}" "${ESPOS_CONFIG_GEN_H}" "${ESPOS_CONFIG_GEN_SCHEMA}"
    COMMAND ${python} "${ESPOS_CONFIG_GEN_SCRIPT}"
            --schema-out "${ESPOS_CONFIG_GEN_SCHEMA}"
            --c-out "${ESPOS_CONFIG_GEN_C}"
            --h-out "${ESPOS_CONFIG_GEN_H}"
            "${descriptors}"
    DEPENDS "${ESPOS_CONFIG_GEN_SCRIPT}" "${descriptors}"
    COMMENT "espos_config: generating schema and descriptor tables"
    COMMAND_EXPAND_LISTS
    VERBATIM)
add_custom_target(espos_config_generate DEPENDS "${ESPOS_CONFIG_GEN_C}" "${ESPOS_CONFIG_GEN_H}")
add_dependencies(${COMPONENT_LIB} espos_config_generate)
set_source_files_properties("${ESPOS_CONFIG_GEN_C}" PROPERTIES GENERATED TRUE)
