cmake_minimum_required(VERSION 3.28)
project(hmmmgc_sdltest LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB SHIM_SRC CONFIGURE_DEPENDS "${HMMMGC_ROOT}/deps/sdl3_shim/*.c")
add_library(SDL3-static STATIC ${SHIM_SRC})
target_include_directories(SDL3-static PUBLIC "${HMMMGC_ROOT}/deps/sdl3_headers/include")
target_include_directories(SDL3-static PRIVATE "${HMMMGC_ROOT}/deps/sdl3_shim")
target_compile_definitions(SDL3-static PRIVATE COS_SWITCH_NRO_DIR="/switch/hmmmgc-nx")
target_link_libraries(SDL3-static PUBLIC nx m)

add_executable(hmmmgc_sdltest main.cpp)
target_link_libraries(hmmmgc_sdltest PRIVATE SDL3-static nx m)
set_target_properties(hmmmgc_sdltest PROPERTIES SUFFIX ".elf")

nx_generate_nacp(hmmmgc_sdltest.nacp NAME "HMMMGC SDL3 Test" AUTHOR "HMMMGC Switch Port" VERSION "0.0.1")
nx_create_nro(hmmmgc_sdltest_nro TARGET hmmmgc_sdltest OUTPUT "${CMAKE_BINARY_DIR}/hmmmgc_sdltest.nro" NACP hmmmgc_sdltest.nacp)
