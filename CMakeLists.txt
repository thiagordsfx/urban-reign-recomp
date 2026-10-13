cmake_minimum_required(VERSION 3.21)

project(UrbanReignPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# runtime/: the PS2Recomp runtime (EE/IOP/VU/GS emulation), recompiler and offline tools, kept in this repository
# with the port's changes (runtime/README.md). thirdparty/: PCSX2's GS and what it needs (thirdparty/README.md).
# The recompiler is prebuilt in tools/ps2_recomp.exe (built from runtime/ps2xRecomp).
# Pass -DURBAN_BUILD_RECOMP=ON to build it from source instead.
option(URBAN_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
# runtime/ps2xTest holds the offline tools: vu1_recomp (VU1 microcode AOT), gs_replay, vu0_audit.
option(URBAN_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
# In-game ImGui "Runtime Debugger" (threads, kernel, IOP/SIF, RPC history, GS, DMA, memory); F1 toggles it.
option(URBAN_DEBUG_UI "Build the ImGui runtime debugger into urban.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${URBAN_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoder for the libmpeg HLE (URBAN_MPEG_HLE=1 only; the FMVs go through the emulated IPU by default).
# A prebuilt LGPL FFmpeg is downloaded at configure time.
option(URBAN_FFMPEG "Decode MPEG video with FFmpeg" ON)
set(PS2X_ENABLE_FFMPEG ${URBAN_FFMPEG} CACHE BOOL "" FORCE)
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory(runtime EXCLUDE_FROM_ALL)
add_subdirectory(src)

# The mod kit's example (modsdk/): ExampleMod.dll in build/port/bin/mods/ExampleMod, to try the mod API.
option(URBAN_BUILD_MOD_EXAMPLE "Build modsdk/example (ExampleMod.dll)" ON)
if(URBAN_BUILD_MOD_EXAMPLE AND WIN32)
    set(URBAN_MODS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/mods")
    add_subdirectory(modsdk/example)
    add_dependencies(urban ExampleMod)
endif()

# The mods that ship with the game (mods/): built into build/port/bin/mods/<Name>.
option(URBAN_BUILD_MODS "Build the mods in mods/ (CoopTool)" ON)
if(URBAN_BUILD_MODS AND WIN32)
    set(URBAN_MODS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/bin/mods")
    add_subdirectory(mods/CoopTool)
    add_dependencies(urban CoopTool)
endif()

# PCSX2's GS as pcsx2gs.dll (docs/GS_PCSX2.md): built with urban, loaded by it when [Video] Renderer=pcsx2.
option(URBAN_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll)" ON)
if(URBAN_PCSX2_GS AND WIN32)
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    add_dependencies(urban pcsx2gs)
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
    target_link_libraries(urban PRIVATE pcsx2gs_api) # the in-game menu panels (src/urban/mods.cpp)
endif()

# URBAN_LTO: whole-program optimization (/GL, linked with /LTCG) of the runtime, the IOP and the port sources. The
# generated code (~10k files) stays out: LTCG over it would blow up link time and memory (docs/ROADMAP.md A4).
option(URBAN_LTO "Whole-program optimization of the runtime, the IOP and the port (not the generated code)" OFF)
if(MSVC AND URBAN_LTO)
    foreach(target ps2_runtime ps2_iop urban)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/GL>)
    endforeach()
    set_property(TARGET ps2_runtime ps2_iop APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
endif()
