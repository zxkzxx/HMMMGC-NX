cmake_minimum_required(VERSION 3.28)
project(hmmmgc_probe LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hmmmgc_probe main.cpp)
target_link_libraries(hmmmgc_probe PRIVATE nx)
set_target_properties(hmmmgc_probe PROPERTIES SUFFIX ".elf")

nx_generate_nacp(hmmmgc_probe.nacp
    NAME "HMMMGC Probe"
    AUTHOR "HMMMGC Switch Port"
    VERSION "0.0.1")

nx_create_nro(hmmmgc_probe_nro
    TARGET hmmmgc_probe
    OUTPUT "${CMAKE_BINARY_DIR}/hmmmgc_probe.nro"
    NACP hmmmgc_probe.nacp)
