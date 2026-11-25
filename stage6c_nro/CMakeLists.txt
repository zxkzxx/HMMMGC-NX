cmake_minimum_required(VERSION 3.25)
project(hmmmgc_nro LANGUAGES C CXX ASM)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 11)

set(SWITCH_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch")
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
target_include_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/include")
target_link_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/lib")
target_link_libraries(ZLIB::ZLIB INTERFACE z)

include("${HMMMGC_ROOT}/deps/ImportDawn.cmake")

add_library(absl_local INTERFACE)
target_include_directories(absl_local INTERFACE "${HMMMGC_ROOT}/deps/dawn_src/third_party/abseil-cpp")
target_link_libraries(absl_local INTERFACE hmmmgc_tint_bundle)
add_library(absl::flat_hash_map ALIAS absl_local)
add_library(absl::btree ALIAS absl_local)

add_library(xxhash_pre STATIC IMPORTED GLOBAL)
set_target_properties(xxhash_pre PROPERTIES
  IMPORTED_LOCATION "${HMMMGC_ROOT}/stage4d_gfxtest/build/libxxhash_lib.a"
  INTERFACE_INCLUDE_DIRECTORIES "${HMMMGC_ROOT}/deps/xxhash")
add_library(xxHash::xxhash ALIAS xxhash_pre)

add_library(SDL3-static STATIC IMPORTED GLOBAL)
set_target_properties(SDL3-static PROPERTIES
  IMPORTED_LOCATION "${HMMMGC_ROOT}/stage5_sdltest/build/libSDL3-static.a"
  INTERFACE_INCLUDE_DIRECTORIES "${HMMMGC_ROOT}/deps/sdl3_headers/include"
  INTERFACE_LINK_LIBRARIES "nx;m")
add_library(SDL3::SDL3-static ALIAS SDL3-static)

add_library(freetype_stub INTERFACE)
add_library(Freetype::Freetype ALIAS freetype_stub)

add_library(tracy_stub INTERFACE)
target_include_directories(tracy_stub INTERFACE "${HMMMGC_ROOT}/deps/tracy_inc")
add_library(Tracy::TracyClient ALIAS tracy_stub)

add_library(imgui STATIC
  ${HMMMGC_ROOT}/deps/imgui/imgui.cpp
  ${HMMMGC_ROOT}/deps/imgui/imgui_draw.cpp
  ${HMMMGC_ROOT}/deps/imgui/imgui_tables.cpp
  ${HMMMGC_ROOT}/deps/imgui/imgui_widgets.cpp
  ${HMMMGC_ROOT}/deps/imgui/misc/cpp/imgui_stdlib.cpp)
target_include_directories(imgui PUBLIC ${HMMMGC_ROOT}/deps/imgui ${HMMMGC_ROOT}/deps/deko)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS AURORA_GFX_DEKO3D=1)
target_compile_options(imgui PRIVATE -O2 -w)

set(AURORA_DAWN_PROVIDER "vendor" CACHE STRING "" FORCE)
set(AURORA_DAWN_LINKAGE "static" CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER "vendor" CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE "static" CACHE STRING "" FORCE)
set(AURORA_ENABLE_GX ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_DVD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD ON CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI OFF CACHE BOOL "" FORCE)
set(AURORA_CACHE_USE_ZSTD OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
set(AURORA_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_VULKAN OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_METAL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D11 OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_D3D12 OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_DESKTOP_GL OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_OPENGLES OFF CACHE BOOL "" FORCE)
set(DAWN_ENABLE_NULL ON CACHE BOOL "" FORCE)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
set(PNG_HARDWARE_OPTIMIZATIONS OFF CACHE BOOL "" FORCE)
set(FMT_TEST OFF CACHE BOOL "" FORCE)
set(FMT_DOC OFF CACHE BOOL "" FORCE)

add_subdirectory("${HMMMGC_ROOT}/deps/aurora_patched" "${CMAKE_BINARY_DIR}/aurora" EXCLUDE_FROM_ALL)

foreach(_t aurora_core aurora_gx aurora_pad aurora_card aurora_vi aurora_os aurora_si aurora_mtx aurora_gd)
  if(TARGET ${_t})
    target_include_directories(${_t} PRIVATE ${HMMMGC_ROOT}/deps/deko ${HMMMGC_ROOT}/deps/deko/aurora ${HMMMGC_ROOT}/deps/deko3d/include)
    target_compile_definitions(${_t} PUBLIC AURORA_GFX_DEKO3D=1)
    target_compile_options(${_t} PRIVATE -O2 -w)
  endif()
endforeach()
file(GLOB DEKO_AURORA_SRCS "${HMMMGC_ROOT}/deps/deko/aurora/*.cpp")
list(FILTER DEKO_AURORA_SRCS EXCLUDE REGEX "test")
target_sources(aurora_gx PRIVATE ${DEKO_AURORA_SRCS})
target_include_directories(aurora_gx PRIVATE ${HMMMGC_ROOT}/deps/aurora_patched/lib ${HMMMGC_ROOT}/deps/deko/aurora)
if(TARGET sqlite3)
  target_sources(sqlite3 PRIVATE ${HMMMGC_ROOT}/deps/sqlite_horizon.c)
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_LOAD_EXTENSION=1 SQLITE_MAX_MMAP_SIZE=0 SQLITE_OMIT_WAL=1 SQLITE_TEMP_STORE=3)
  target_compile_options(sqlite3 PRIVATE -O2 -w)
endif()

file(GLOB_RECURSE DEKO_SHADERS "${HMMMGC_ROOT}/stage4d_gfxtest/build/CMakeFiles/cos_switch_deko_shaders.dir/*.o*")

add_executable(hmmmgc switch_anchor.cpp ${DEKO_SHADERS})
set_target_properties(hmmmgc PROPERTIES SUFFIX ".elf")
target_link_options(hmmmgc PRIVATE "LINKER:--wrap=pthread_create" "LINKER:--wrap=wgpuBindGroupAddRef" "LINKER:--wrap=wgpuBindGroupRelease" "LINKER:--wrap=wgpuDeviceCreateBindGroup" "LINKER:--wrap=wgpuDeviceCreateSampler" "LINKER:--wrap=wgpuDeviceCreateTexture" "LINKER:--wrap=wgpuSamplerAddRef" "LINKER:--wrap=wgpuSamplerRelease" "LINKER:--wrap=wgpuTextureAddRef" "LINKER:--wrap=wgpuTextureCreateView" "LINKER:--wrap=wgpuTextureRelease" "LINKER:--wrap=wgpuTextureViewAddRef" "LINKER:--wrap=wgpuTextureViewRelease")
target_link_libraries(hmmmgc PRIVATE
  "-Wl,--whole-archive"
  "${HMMMGC_ROOT}/stage6b_runtime/build/libhmmmgc_runtime.a"
  "${HMMMGC_ROOT}/stage6_hmmmgc/build/libhmmmgc_game.a"
  "${HMMMGC_ROOT}/stage6_hmmmgc/build/libhmmmgc_mods.a"
  "-Wl,--no-whole-archive"
  "$<LINK_GROUP:RESCAN,aurora::core;aurora::gx;aurora::vi;aurora::pad;aurora::card;aurora::os;aurora::si;aurora::mtx;aurora::gd;${HMMMGC_ROOT}/stage4d_gfxtest/build/libcos_switch_deko.a;${HMMMGC_ROOT}/stage4d_gfxtest/build/deko3d_build/libdeko3d_release.a;uamlib;SDL3::SDL3-static;imgui;sqlite3;xxHash::xxhash;fmt::fmt>"
  dawn::webgpu_dawn PNG::PNG ZLIB::ZLIB nx m)

nx_generate_nacp(hmmmgc-nx.nacp
  NAME "Harvest Moon: Magical Melody"
  AUTHOR "zxkzxx"
  VERSION "0.1.0")
nx_create_nro(hmmmgc_nro
  TARGET hmmmgc
  OUTPUT "${CMAKE_BINARY_DIR}/hmmmgc-nx.nro"
  ICON "${CMAKE_CURRENT_SOURCE_DIR}/icon.jpg"
  NACP hmmmgc-nx.nacp)
