# SPDX-License-Identifier: Apache-2.0
#
# Included by the IDF build system for every project that has espos_config in
# its component list, before any component CMakeLists.txt is processed. It
# provides the registration hook that other components call (AFTER their
# idf_component_register) to contribute a config descriptor:
#
#     espos_config_add_descriptor(config/myns.json)
#
# The registered files are merged by tools/espos_gen_config.py into the JSON
# Schema and the C descriptor tables compiled into espos_config. Registration
# is order-independent: the generator command reads the list through a
# generator expression, i.e. after every component has been processed (the
# same mechanism IDF's ldgen uses for linker fragments).

function(espos_config_add_descriptor file)
    if(NOT IS_ABSOLUTE "${file}")
        set(file "${CMAKE_CURRENT_LIST_DIR}/${file}")
    endif()
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "espos_config_add_descriptor: '${file}' does not exist")
    endif()
    idf_build_set_property(ESPOS_CONFIG_DESCRIPTORS "${file}" APPEND)
endfunction()
