cmake_minimum_required(VERSION 3.28)
project(hmmmgc_memtest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(hmmmgc_memtest main.cpp)
target_compile_options(hmmmgc_memtest PRIVATE -O2 -fno-fast-math -ffp-contract=off)
target_link_libraries(hmmmgc_memtest PRIVATE nx)
set_target_properties(hmmmgc_memtest PROPERTIES SUFFIX ".elf")

nx_generate_nacp(hmmmgc_memtest.nacp
    NAME "HMMMGC MemTest"
    AUTHOR "HMMMGC Switch Port"
    VERSION "0.0.1")

nx_create_nro(hmmmgc_memtest_nro
    TARGET hmmmgc_memtest
    OUTPUT "${CMAKE_BINARY_DIR}/hmmmgc_memtest.nro"
    NACP hmmmgc_memtest.nacp)
