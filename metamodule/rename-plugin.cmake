# Run after the SDK packages the plugin (see CMakeLists.txt). Renames
#   <DIR>/<SLUG>.mmplugin  ->  <DIR>/<SLUG>-v<VERSION>.mmplugin
# which is the file name 4ms requires for released plugins. Older versioned files
# are removed first, so the folder only ever holds one copy of the plugin to put
# on the MetaModule.
file(GLOB old_files "${DIR}/${SLUG}-v*.mmplugin")
if(old_files)
    file(REMOVE ${old_files})
endif()
file(RENAME "${DIR}/${SLUG}.mmplugin" "${DIR}/${SLUG}-v${VERSION}.mmplugin")
message(STATUS "Renamed plugin to ${DIR}/${SLUG}-v${VERSION}.mmplugin")
