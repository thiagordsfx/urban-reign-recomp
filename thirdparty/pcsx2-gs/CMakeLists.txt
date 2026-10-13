if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.21)
    project(pcsx2_gs_standalone LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

# PCSX2's GS (hardware renderer) as libraries for the port (docs/GS_PCSX2.md). GPL-3, from github.com/PCSX2/pcsx2.
# thirdparty/pcsx2: only the PCSX2 files this build compiles or includes (thirdparty/README.md says which commit and how
# the list was made). Plain variables, not cache entries: an old cache pointing at a full PCSX2 clone does not win.
set(P "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2")
# DirectX-Headers (github.com/microsoft/DirectX-Headers, MIT): the "directx/d3d12.h" PCSX2 includes.
set(DIRECTX_HEADERS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../directx-headers")

# Flags PCSX2 builds with (cmake/BuildParameters.cmake, pcsx2/CMakeLists.txt). Exceptions stay on (PCSX2 turns them
# off; its code does not throw, and the rest of urban.exe uses them).
add_library(pcsx2_flags INTERFACE)
target_compile_features(pcsx2_flags INTERFACE cxx_std_20)
target_compile_definitions(pcsx2_flags INTERFACE
    _M_X86=1 __SSE4_1__=1
    WINVER=0x0A00 _WIN32_WINNT=0x0A00 WIN32_LEAN_AND_MEAN WIL_SUPPRESS_EXCEPTIONS
    _CRT_NONSTDC_NO_WARNINGS _CRT_SECURE_NO_WARNINGS CRT_SECURE_NO_DEPRECATE _SCL_SECURE_NO_WARNINGS
    _UNICODE UNICODE NOMINMAX
    IMGUI_USER_CONFIG="pcsx2gs_imconfig.h")
target_compile_options(pcsx2_flags INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/Zc:externConstexpr /Zc:__cplusplus /permissive- /Zc:preprocessor>
    /utf-8 /Zo /fp:contract /GS- /W0 /bigobj /MP)
target_include_directories(pcsx2_flags INTERFACE
    "${P}" "${P}/pcsx2" "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${P}/3rdparty/fmt/include" "${P}/3rdparty/imgui/include" "${P}/3rdparty/xbyak"
    "${P}/3rdparty/fast_float/include" "${P}/3rdparty/d3d12memalloc/include" "${P}/3rdparty/winwil/include"
    "${P}/3rdparty/cpuinfo/include" "${P}/3rdparty/include"
    "${DIRECTX_HEADERS_DIR}/include")

# --- third party ---
add_library(pcsx2_fmt STATIC "${P}/3rdparty/fmt/src/format.cc" "${P}/3rdparty/fmt/src/os.cc")
target_link_libraries(pcsx2_fmt PRIVATE pcsx2_flags)

add_library(pcsx2_imgui STATIC
    "${P}/3rdparty/imgui/src/imgui.cpp" "${P}/3rdparty/imgui/src/imgui_draw.cpp"
    "${P}/3rdparty/imgui/src/imgui_tables.cpp" "${P}/3rdparty/imgui/src/imgui_widgets.cpp"
    "${P}/3rdparty/imgui/src/imgui_stdlib.cpp")
target_link_libraries(pcsx2_imgui PRIVATE pcsx2_flags)

add_library(pcsx2_d3d12ma STATIC "${P}/3rdparty/d3d12memalloc/src/D3D12MemAlloc.cpp")
target_link_libraries(pcsx2_d3d12ma PRIVATE pcsx2_flags)

set(CPUINFO_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_UNIT_TESTS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_MOCK_TESTS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CPUINFO_BUILD_PKG_CONFIG OFF CACHE BOOL "" FORCE)
set(CPUINFO_LIBRARY_TYPE static CACHE STRING "" FORCE)
add_subdirectory("${P}/3rdparty/cpuinfo" "${CMAKE_CURRENT_BINARY_DIR}/cpuinfo" EXCLUDE_FROM_ALL)

# --- common/ (PCSX2's utility library), minus what the GS does not need (emitter, HTTP, crash handler, images) ---
set(COMMON_SOURCES
    AlignedMalloc.cpp Assertions.cpp Console.cpp DynamicLibrary.cpp Error.cpp FileSystem.cpp HostSys.cpp
    MemorySettingsInterface.cpp MD5Digest.cpp Perf.cpp ProgressCallback.cpp ReadbackSpinManager.cpp Semaphore.cpp
    SettingsWrapper.cpp SmallString.cpp StringUtil.cpp TextureDecompress.cpp Timer.cpp WindowInfo.cpp
    Windows/WinThreads.cpp Windows/WinHostSys.cpp Windows/WinMisc.cpp)
list(TRANSFORM COMMON_SOURCES PREPEND "${P}/common/")
add_library(pcsx2_common STATIC ${COMMON_SOURCES})
target_link_libraries(pcsx2_common PRIVATE pcsx2_flags cpuinfo)

# --- GS (pcsx2/CMakeLists.txt: pcsx2GSSources + pcsx2GSSourcesUnshared, single ISA, DX11 + DX12) ---
# Left out: GSDump/GSLzma (GS dumps, zstd/xz), GSCapture (FFmpeg video capture), GSPng (libpng snapshots); compat/
# has empty versions.
set(GS_SOURCES
    GS/GS.cpp GS/GSClut.cpp GS/GSDrawingContext.cpp GS/GSDrawingEnvironment.cpp GS/GSLocalMemory.cpp
    GS/GSPerfMon.cpp GS/GSRingHeap.cpp GS/GSState.cpp GS/GSTables.cpp GS/GSUtil.cpp GS/GSVector.cpp GS/MultiISA.cpp
    GS/Renderers/Common/GSDevice.cpp GS/Renderers/Common/GSDirtyRect.cpp GS/Renderers/Common/GSFunctionMap.cpp
    GS/Renderers/Common/GSRenderer.cpp GS/Renderers/Common/GSTexture.cpp GS/Renderers/Common/GSVertexTrace.cpp
    GS/Renderers/Null/GSRendererNull.cpp GS/Renderers/HW/GSHwHack.cpp GS/Renderers/HW/GSRendererHW.cpp
    GS/Renderers/HW/GSTextureCache.cpp GS/Renderers/SW/GSTextureCacheSW.cpp
    GS/GSBlock.cpp GS/GSLocalMemoryMultiISA.cpp GS/GSXXH.cpp GS/Renderers/Common/GSVertexTraceFMM.cpp
    GS/Renderers/HW/GSRendererHWMultiISA.cpp GS/Renderers/SW/GSDrawScanline.cpp GS/Renderers/SW/GSRasterizer.cpp
    GS/Renderers/SW/GSRendererSW.cpp GS/Renderers/SW/GSDrawScanlineCodeGenerator.all.cpp
    GS/Renderers/SW/GSSetupPrimCodeGenerator.all.cpp
    GS/Renderers/DX11/D3D.cpp GS/Renderers/DX11/D3D11ShaderCache.cpp GS/Renderers/DX11/GSDevice11.cpp
    GS/Renderers/DX11/GSTexture11.cpp GS/Renderers/DX12/D3D12Builders.cpp GS/Renderers/DX12/D3D12DescriptorHeapManager.cpp
    GS/Renderers/DX12/D3D12ShaderCache.cpp GS/Renderers/DX12/D3D12StreamBuffer.cpp GS/Renderers/DX12/GSDevice12.cpp
    GS/Renderers/DX12/GSTexture12.cpp GS/Renderers/HW/GSTextureReplacements.cpp)
list(TRANSFORM GS_SOURCES PREPEND "${P}/pcsx2/")
# Texture replacement loaders: PCSX2's file with PNG through WIC instead of libpng (tools/make_loaders.py); .zip
# texture packs are left out (compat/zip.h).
list(APPEND GS_SOURCES compat/GSTextureReplacementLoaders.cpp)
add_library(pcsx2_gs STATIC ${GS_SOURCES})
target_link_libraries(pcsx2_gs PRIVATE pcsx2_flags)
target_link_libraries(pcsx2_gs PUBLIC pcsx2_common pcsx2_fmt pcsx2_imgui pcsx2_d3d12ma cpuinfo
    d3d11 d3d12 dxgi d3dcompiler dxguid winmm windowscodecs ole32)
target_include_directories(pcsx2_gs INTERFACE
    "${P}" "${P}/pcsx2" "${CMAKE_CURRENT_SOURCE_DIR}/compat" "${P}/3rdparty/fmt/include")

# --- our side: the host/VM functions the GS calls (phase 1: minimal) and the device test ---
add_library(pcsx2gs_api INTERFACE)
target_include_directories(pcsx2gs_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(pcsx2_gs_host STATIC compat/HostStubs.cpp compat/Pcsx2ConfigGS.cpp)
target_link_libraries(pcsx2_gs_host PRIVATE pcsx2_flags)
target_link_libraries(pcsx2_gs_host PUBLIC pcsx2gs_api) # the menu types (pcsx2gs_host.h)
target_link_libraries(pcsx2_gs PUBLIC onecore pathcch dxcompiler delayimp)
# dxcompiler.dll (DX12's shader compiler) only loads when the DX12 renderer is used.
target_link_options(pcsx2_gs INTERFACE /DELAYLOAD:dxcompiler.dll)

add_executable(pcsx2_gs_test test/GSTest.cpp)
target_link_libraries(pcsx2_gs_test PRIVATE pcsx2_flags pcsx2_gs pcsx2_gs_host)

# pcsx2gs.dll: the GS behind the C interface of include/pcsx2gs_api.h (phase 2). urban.exe loads it at run time, so
# PCSX2's ImGui, fmt etc. stay inside the DLL. Next to it: resources/shaders (read at run time by the devices).
add_library(pcsx2gs SHARED compat/Bridge.cpp)
target_link_libraries(pcsx2gs PRIVATE pcsx2_flags pcsx2gs_api pcsx2_gs pcsx2_gs_host)
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set_target_properties(pcsx2gs PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
endif()
# resources/ (shaders, OSD font; see resources/README.md) next to the DLL, and the shader cache made from the game's
# menus and fights (shadercache/) when the player has none yet: the first fights then compile almost nothing.
add_custom_command(TARGET pcsx2gs POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:pcsx2gs>/resources"
    COMMAND ${CMAKE_COMMAND} -DSRC="${CMAKE_CURRENT_SOURCE_DIR}/shadercache" -DDST="$<TARGET_FILE_DIR:pcsx2gs>/shadercache_pcsx2"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/copy_if_missing.cmake")
