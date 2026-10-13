# URBAN_ARCH: instruction set the compiler may use in the generated code (game code + VU1/VU0 AOT, the bulk of the build).
# SSE41 (default): the PCSX2 minimum; one urban.exe for every CPU (docs/ROADMAP.md A4). MSVC 14.36 has no /arch:SSE4.2
# (it ignores it, D9002), so the compiler's own code stays SSE2 and the runtime's explicit SSE4.1 intrinsics still apply.
# AVX2: only to compare speed; the exe then needs an AVX2 CPU. No FMA contraction either way (/fp:precise, checked).
set(URBAN_ARCH "SSE41" CACHE STRING "Instruction set of the generated code: SSE41 (default) or AVX2 (comparison only)")
set_property(CACHE URBAN_ARCH PROPERTY STRINGS SSE41 AVX2)
if(URBAN_ARCH STREQUAL "AVX2")
    set(URBAN_ARCH_FLAG /arch:AVX2)
else()
    set(URBAN_ARCH_FLAG "")
endif()

set(URBAN_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")
set(URBAN_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")

if(NOT EXISTS "${URBAN_GENERATED_DIR}/register_functions.cpp")
    message(FATAL_ERROR
        "Recompiled game code not found in ${URBAN_GENERATED_DIR}.\n"
        "Put your own disc image in game/ and run: python build.py generate")
endif()

file(GLOB URBAN_GENERATED_SOURCES CONFIGURE_DEPENDS "${URBAN_GENERATED_DIR}/*.cpp")

set(URBAN_HOST_SOURCES
    main.cpp
    urban/function_hooks.cpp
    urban/call_profile.cpp
    urban/call_trace.cpp
    urban/game_disc.cpp
    urban/hang_dump.cpp
    urban/anim_decoder.cpp
    urban/hle_bindings.cpp
    urban/log.cpp
    urban/mods.cpp
    urban/mod_loader.cpp
    urban/native_profile.cpp
    urban/sha256.cpp
    urban/thread_policy.cpp
)

# Reuse an already-built game code library when only host sources change (the full EE build is long).
set(URBAN_GAME_CODE_LIBRARY "" CACHE FILEPATH "Reuse an unchanged generated EE library for host-only builds")
if(URBAN_GAME_CODE_LIBRARY)
    if(NOT EXISTS "${URBAN_GAME_CODE_LIBRARY}")
        message(FATAL_ERROR "Cached generated EE library does not exist: ${URBAN_GAME_CODE_LIBRARY}")
    endif()
    add_library(urban_game_code STATIC IMPORTED)
    set_target_properties(urban_game_code PROPERTIES IMPORTED_LOCATION "${URBAN_GAME_CODE_LIBRARY}")
    target_include_directories(urban_game_code INTERFACE "${URBAN_GENERATED_DIR}")
    target_link_libraries(urban_game_code INTERFACE ps2_runtime)
    # The huge-function sources live in a sibling library built next to it.
    get_filename_component(URBAN_GAME_CODE_LIBRARY_DIR "${URBAN_GAME_CODE_LIBRARY}" DIRECTORY)
    target_link_libraries(urban_game_code INTERFACE "${URBAN_GAME_CODE_LIBRARY_DIR}/urban_game_code_huge.lib")
else()
    # /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip. Fine for most
    # files, but sub_001A7AE8 (~11k guest instructions, 2 MB of C++) ran over 40 minutes with it. Files above the
    # threshold go into urban_game_code_huge, which keeps MSVC's default huge-function handling.
    set(URBAN_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
    set(URBAN_HUGE_SOURCES "")
    foreach(source IN LISTS URBAN_GENERATED_SOURCES)
        file(SIZE "${source}" sourceBytes)
        if(sourceBytes GREATER URBAN_HUGE_SOURCE_BYTES AND NOT source MATCHES "register_functions\.cpp$")
            list(APPEND URBAN_HUGE_SOURCES "${source}")
        endif()
    endforeach()
    list(REMOVE_ITEM URBAN_GENERATED_SOURCES ${URBAN_HUGE_SOURCES})

    add_library(urban_game_code_huge STATIC ${URBAN_HUGE_SOURCES})
    target_include_directories(urban_game_code_huge PUBLIC "${URBAN_GENERATED_DIR}")
    target_link_libraries(urban_game_code_huge PUBLIC ps2_runtime)
    # Each huge file needs several GB in MSVC's pass 2; compiling them all at once runs out of heap (C1002).
    set(URBAN_HUGE_JOBS 2 CACHE STRING "Concurrent compiles of the huge generated sources (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS urban_huge=${URBAN_HUGE_JOBS})
    set_property(TARGET urban_game_code_huge PROPERTY JOB_POOL_COMPILE urban_huge)
    if(MSVC)
        target_compile_options(urban_game_code_huge PRIVATE /bigobj /fp:precise ${URBAN_ARCH_FLAG} /GS- /W0)
        # Without /d2OptimizeHugeFunctions MSVC does not optimize these functions at all (C4883): the animation decoder
        # sub_00292670 ran at ~17 ns per guest instruction, /Od-like code with every temporary on a 400 KB stack.
        # The huge files that do compile in reasonable time get it back; sub_001A7AE8 (40+ minutes) does not.
        set(URBAN_HUGE_OPTIMIZED "sub_00292670_0x292670.cpp;sub_00299260_0x299260.cpp;sub_001A1948_0x1a1948.cpp"
            CACHE STRING "Huge generated files still compiled with /d2OptimizeHugeFunctions")
        foreach(source IN LISTS URBAN_HUGE_SOURCES)
            get_filename_component(sourceName "${source}" NAME)
            if(sourceName IN_LIST URBAN_HUGE_OPTIMIZED)
                set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/d2OptimizeHugeFunctions")
            endif()
        endforeach()
    endif()

    add_library(urban_game_code STATIC ${URBAN_GENERATED_SOURCES})
    target_link_libraries(urban_game_code PUBLIC urban_game_code_huge)
    target_include_directories(urban_game_code PUBLIC "${URBAN_GENERATED_DIR}")
    target_link_libraries(urban_game_code PUBLIC ps2_runtime)
    set_target_properties(urban_game_code PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(urban_game_code PRIVATE
        "${URBAN_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${URBAN_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${URBAN_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${URBAN_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${URBAN_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${URBAN_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        target_compile_options(urban_game_code PRIVATE /bigobj /fp:precise ${URBAN_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
        # One ~100k-line table initializer that runs once at startup: compiles in seconds without PCH/optimizer,
        # can take hours with them.
        set_source_files_properties("${URBAN_GENERATED_DIR}/register_functions.cpp" PROPERTIES
            COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

add_executable(urban ${URBAN_HOST_SOURCES})
target_include_directories(urban PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${URBAN_GENERATED_DIR}" "${CMAKE_SOURCE_DIR}/modsdk/include")
target_link_libraries(urban PRIVATE urban_game_code ps2_runtime)

if(MSVC)
    target_compile_options(urban PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(urban PRIVATE _CRT_SECURE_NO_WARNINGS URBAN_ARCH_NAME="${URBAN_ARCH}")
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    target_link_options(urban PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    target_link_libraries(urban PRIVATE dbghelp winmm)
endif()
if(WIN32)
    target_sources(urban PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/res/urban.rc")
endif()

if(URBAN_DEBUG_UI AND TARGET rlImGui)
    target_sources(urban PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_include_directories(urban PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
    target_link_libraries(urban PRIVATE rlImGui imgui)
    target_compile_definitions(urban PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(urban)
endif()

# VU1 microcode compiled ahead of time (as in the SotC port): capture the code images with PS2X_VU1_CAPTURE, then
# run vu1_recomp (configure with -DURBAN_BUILD_TOOLS=ON) into generated/vu1. Images not compiled stay interpreted.
file(GLOB URBAN_VU1_SOURCES CONFIGURE_DEPENDS "${URBAN_GENERATED_DIR}/vu1/*.cpp")
if(URBAN_VU1_SOURCES)
    add_library(urban_vu1_code STATIC ${URBAN_VU1_SOURCES})
    target_include_directories(urban_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(urban_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(urban PRIVATE urban_vu1_code)
    target_compile_definitions(urban PRIVATE URBAN_HAS_VU1_PROGRAMS=1)
    if(MSVC)
        # Without /d2OptimizeHugeFunctions MSVC left the big fastBlock functions unoptimized (C4883): block 9, the vertex
        # program that dominates fights, had a 462 KB stack frame and spilled every temporary (tools/frame_scan.py).
        target_compile_options(urban_vu1_code PRIVATE /bigobj /fp:precise ${URBAN_ARCH_FLAG} /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

set_target_properties(urban PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
