cmake_minimum_required(VERSION 3.22)

# Location of the MetaModule plugin SDK (path must not contain spaces).
# Override with -DMETAMODULE_SDK_DIR=... or the METAMODULE_SDK_DIR environment variable.
if(NOT "${METAMODULE_SDK_DIR}" STREQUAL "")
    message("METAMODULE_SDK_DIR set by CMake variable ${METAMODULE_SDK_DIR}")
elseif(DEFINED ENV{METAMODULE_SDK_DIR})
    set(METAMODULE_SDK_DIR "$ENV{METAMODULE_SDK_DIR}")
else()
    set(METAMODULE_SDK_DIR "C:/metamodule-plugin-sdk")
endif()
include(${METAMODULE_SDK_DIR}/plugin.cmake)

project(QuantizerSequencer VERSION 2.0.0 DESCRIPTION "QSeq 8 for MetaModule" LANGUAGES C CXX ASM)

# The MetaModule build compiles the same sources as the VCV Rack build.
set(SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(QuantizerSequencer STATIC)
target_sources(QuantizerSequencer PRIVATE
    ${SOURCE_DIR}/src/plugin.cpp
    ${SOURCE_DIR}/src/QuantizerSequencer.cpp
)
target_include_directories(QuantizerSequencer PRIVATE ${SOURCE_DIR}/src)

create_plugin(
    SOURCE_LIB      QuantizerSequencer
    PLUGIN_NAME     QuantizerSequencer
    PLUGIN_JSON     ${SOURCE_DIR}/plugin.json
    SOURCE_ASSETS   ${CMAKE_CURRENT_LIST_DIR}/assets
    DESTINATION     ${CMAKE_CURRENT_LIST_DIR}/metamodule-plugins
)
