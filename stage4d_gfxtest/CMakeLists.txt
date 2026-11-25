cmake_minimum_required(VERSION 3.28)
project(hmmmgc_gfxtest LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SWITCH_PORTLIBS "$ENV{DEVKITPRO}/portlibs/switch")
add_library(ZLIB::ZLIB INTERFACE IMPORTED GLOBAL)
target_include_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/include")
target_link_directories(ZLIB::ZLIB INTERFACE "${SWITCH_PORTLIBS}/lib")
target_link_libraries(ZLIB::ZLIB INTERFACE z)

include("${HMMMGC_ROOT}/deps/ImportDawn.cmake")

add_library(xxhash_lib STATIC ${HMMMGC_ROOT}/deps/xxhash/xxhash.c)
target_include_directories(xxhash_lib PUBLIC ${HMMMGC_ROOT}/deps/xxhash)
add_library(xxHash::xxhash ALIAS xxhash_lib)

add_library(imgui STATIC
    ${HMMMGC_ROOT}/deps/imgui/imgui.cpp
    ${HMMMGC_ROOT}/deps/imgui/imgui_draw.cpp
    ${HMMMGC_ROOT}/deps/imgui/imgui_tables.cpp
    ${HMMMGC_ROOT}/deps/imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC ${HMMMGC_ROOT}/deps/imgui)
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

add_subdirectory(${HMMMGC_ROOT}/deps/deko3d deko3d_build EXCLUDE_FROM_ALL)

set(_dk_dir "${HMMMGC_ROOT}/deps/deko")
file(GLOB _dk_glsl CONFIGURE_DEPENDS "${_dk_dir}/shaders/*.glsl")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/dksh")
set(_dk_dksh)
foreach(_glsl IN LISTS _dk_glsl)
    get_filename_component(_name "${_glsl}" NAME_WE)
    if(_name MATCHES "_vsh$")
        set(_stage vert)
    else()
        set(_stage frag)
    endif()
    nx_add_shader_program(hmmmgc_dksh_${_name} "${_glsl}" ${_stage}
        OUTPUT "${CMAKE_BINARY_DIR}/dksh/${_name}.dksh")
    list(APPEND _dk_dksh hmmmgc_dksh_${_name})
endforeach()
dkp_add_embedded_binary_library(cos_switch_deko_shaders ${_dk_dksh})

add_library(cos_switch_deko STATIC
    "${_dk_dir}/device.cpp"
    "${_dk_dir}/memory.cpp"
    "${_dk_dir}/descriptors.cpp"
    "${_dk_dir}/overlay.cpp"
    "${_dk_dir}/pattern.cpp"
    "${_dk_dir}/shader_cache.cpp"
    "${_dk_dir}/shaders.cpp"
    "${_dk_dir}/shader_translate.cpp"
    "${_dk_dir}/dksh_file.cpp")
target_include_directories(cos_switch_deko PUBLIC "${_dk_dir}")
target_include_directories(cos_switch_deko PRIVATE "${HMMMGC_DAWN_SRC}" "${HMMMGC_DAWN_SRC}/include")
target_compile_definitions(cos_switch_deko PRIVATE
    COS_SWITCH_NRO_DIR="sdmc:/switch/hmmmgc-nx"
    COS_SWITCH_DATA_DIR="sdmc:/switch/hmmmgc-nx"
    COS_DK_DEBUG_LIB=1)
target_link_libraries(cos_switch_deko
    PRIVATE cos_switch_deko_shaders imgui ZLIB::ZLIB xxHash::xxhash uamlib
            tint_api tint_lang_glsl_writer tint_lang_wgsl_reader tint_lang_wgsl_inspector
    PUBLIC deko3d_release nx m)

add_executable(hmmmgc_gfxtest main.cpp)
target_link_libraries(hmmmgc_gfxtest PRIVATE cos_switch_deko nx m)
target_link_options(hmmmgc_gfxtest PRIVATE "LINKER:--wrap=pthread_create")
set_target_properties(hmmmgc_gfxtest PROPERTIES SUFFIX ".elf")

nx_generate_nacp(hmmmgc_gfxtest.nacp
    NAME "HMMMGC Deko3D Test"
    AUTHOR "HMMMGC Switch Port"
    VERSION "0.0.1")
nx_create_nro(hmmmgc_gfxtest_nro
    TARGET hmmmgc_gfxtest
    OUTPUT "${CMAKE_BINARY_DIR}/hmmmgc_gfxtest.nro"
    NACP hmmmgc_gfxtest.nacp)
