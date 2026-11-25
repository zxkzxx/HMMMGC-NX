cmake_minimum_required(VERSION 3.28)
project(hmmmgc_dawn LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

string(APPEND CMAKE_C_FLAGS " -fno-omit-frame-pointer")
string(APPEND CMAKE_CXX_FLAGS " -fno-omit-frame-pointer")

include("${HMMMGC_ROOT}/deps/dawn_cfg/dawn.cmake")
