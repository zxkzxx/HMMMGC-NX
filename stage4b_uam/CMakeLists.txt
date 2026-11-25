cmake_minimum_required(VERSION 3.28)
project(stage4b_uam LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(SWITCH_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch")
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
target_include_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/include")
target_link_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/lib")
target_link_libraries(ZLIB::ZLIB INTERFACE z)

add_subdirectory(${HMMMGC_ROOT}/deps/uam uam_build EXCLUDE_FROM_ALL)
