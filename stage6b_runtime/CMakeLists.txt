cmake_minimum_required(VERSION 3.25)
project(hmmmgc_rt LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(R "${HMMMGC_ROOT}/src/hmmmgc")
file(GLOB_RECURSE RT_SRC CONFIGURE_DEPENDS "${R}/runtime/src/*.cpp")
add_library(hmmmgc_runtime STATIC ${RT_SRC} "${CMAKE_CURRENT_SOURCE_DIR}/switch_glue.cpp")
include("${R}/shared/shared.cmake")
add_shared_ui(hmmmgc_runtime)
target_compile_definitions(hmmmgc_runtime PRIVATE __SWITCH__=1 HMMMGC_SWITCH=1 TARGET_PC=1 HMMM_FAR_CAMERA_MOD)
target_include_directories(hmmmgc_runtime PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/shim_inc" "${R}/runtime/src" "${R}/runtime/mod_api"
  "${HMMMGC_ROOT}/stage6_hmmmgc/build/generated" "${HMMMGC_ROOT}/deps/wiicompiled/runtime/include"
  "${HMMMGC_ROOT}/deps/aurora_patched/include" "${HMMMGC_ROOT}/deps/sdl3_headers/include"
  "${HMMMGC_ROOT}/deps/imgui" "${HMMMGC_ROOT}/deps")
target_compile_options(hmmmgc_runtime PRIVATE -fno-fast-math -ffp-contract=off -O3 -fomit-frame-pointer -w)
