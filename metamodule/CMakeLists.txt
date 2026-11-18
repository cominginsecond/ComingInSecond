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

project(ComingInSecond VERSION 2.0.0 DESCRIPTION "Coming in Second modules for MetaModule" LANGUAGES C CXX ASM)

# The MetaModule build compiles the same sources as the VCV Rack build.
set(SOURCE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)

add_library(ComingInSecond STATIC)
target_sources(ComingInSecond PRIVATE
    ${SOURCE_DIR}/src/plugin.cpp
    ${SOURCE_DIR}/src/QuantizerSequencer.cpp
    ${SOURCE_DIR}/src/VibroLFO.cpp
)
target_include_directories(ComingInSecond PRIVATE ${SOURCE_DIR}/src)

create_plugin(
    SOURCE_LIB      ComingInSecond
    PLUGIN_NAME     ComingInSecond   # must match the slug in plugin.json
    PLUGIN_JSON     ${SOURCE_DIR}/plugin.json
    SOURCE_ASSETS   ${CMAKE_CURRENT_LIST_DIR}/assets
    DESTINATION     ${CMAKE_CURRENT_LIST_DIR}/metamodule-plugins
)

# The SDK always writes <slug>.mmplugin. Rename it to <slug>-v<version>.mmplugin, the
# name 4ms requires for releases, using the version from plugin.json. PLUGIN_NAME itself
# must stay equal to the slug, since it is also the folder the plugin unpacks into.
file(READ ${SOURCE_DIR}/plugin.json PLUGIN_JSON_TEXT)
string(JSON PLUGIN_SLUG GET "${PLUGIN_JSON_TEXT}" slug)
string(JSON PLUGIN_VERSION GET "${PLUGIN_JSON_TEXT}" version)
# Re-run this configure step when plugin.json changes, so a version bump is picked up.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${SOURCE_DIR}/plugin.json)

add_custom_command(TARGET plugin POST_BUILD
    COMMAND ${CMAKE_COMMAND}
        -DDIR=${CMAKE_CURRENT_LIST_DIR}/metamodule-plugins
        -DSLUG=${PLUGIN_SLUG}
        -DVERSION=${PLUGIN_VERSION}
        -P ${CMAKE_CURRENT_LIST_DIR}/rename-plugin.cmake
    VERBATIM
)
