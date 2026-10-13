# ExampleMod: builds on its own (cmake -S modsdk/example -B build) or as part of the game's build
# (URBAN_BUILD_MOD_EXAMPLE, the default), where it lands in build/port/bin/mods/ExampleMod.
cmake_minimum_required(VERSION 3.21)
project(ExampleMod C)

add_library(ExampleMod SHARED example_mod.c)
target_include_directories(ExampleMod PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")
set_target_properties(ExampleMod PROPERTIES PREFIX "" OUTPUT_NAME "ExampleMod")
if(DEFINED URBAN_MODS_OUTPUT_DIR)
    set_target_properties(ExampleMod PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${URBAN_MODS_OUTPUT_DIR}/ExampleMod")
    add_custom_command(TARGET ExampleMod POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/mod.json" "$<TARGET_FILE_DIR:ExampleMod>/mod.json")
endif()
