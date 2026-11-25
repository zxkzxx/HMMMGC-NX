include_guard(GLOBAL)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(CMAKE_C_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_C_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "LINKER:--start-group" "LINKER:--end-group")


set(HMMMGC_DAWN_SRC "${HMMMGC_ROOT}/deps/dawn_src")
set(HMMMGC_DAWN_BUILD "${HMMMGC_ROOT}/stage4c_dawn/build/_deps/dawn-build")
set(HMMMGC_DAWN_GEN_INC "${HMMMGC_DAWN_BUILD}/gen/include")

file(GLOB_RECURSE _hmmmgc_tint_libs
    "${HMMMGC_DAWN_BUILD}/src/tint/*.a"
    "${HMMMGC_DAWN_BUILD}/src/utils/*.a"
    "${HMMMGC_DAWN_BUILD}/third_party/abseil/*.a")

add_library(hmmmgc_tint_bundle INTERFACE)
target_include_directories(hmmmgc_tint_bundle INTERFACE
    "${HMMMGC_DAWN_GEN_INC}"
    "${HMMMGC_DAWN_SRC}/include"
    "${HMMMGC_DAWN_SRC}")
target_link_libraries(hmmmgc_tint_bundle INTERFACE
    "$<LINK_GROUP:RESCAN,${_hmmmgc_tint_libs}>")

foreach(_t tint_api tint_lang_glsl_writer tint_lang_wgsl_reader tint_lang_wgsl_inspector)
    if(NOT TARGET ${_t})
        add_library(${_t} INTERFACE)
        target_link_libraries(${_t} INTERFACE hmmmgc_tint_bundle)
    endif()
endforeach()

file(GLOB_RECURSE _hmmmgc_dawn_libs
    "${HMMMGC_DAWN_BUILD}/src/dawn/*.a")

if(NOT TARGET webgpu_dawn)
    add_library(webgpu_dawn INTERFACE)
    target_include_directories(webgpu_dawn INTERFACE
        "${HMMMGC_DAWN_GEN_INC}"
        "${HMMMGC_DAWN_SRC}/include")
    target_link_libraries(webgpu_dawn INTERFACE
        "$<LINK_GROUP:RESCAN,${_hmmmgc_dawn_libs};${_hmmmgc_tint_libs}>")
endif()
if(NOT TARGET dawn::webgpu_dawn)
    add_library(dawn::webgpu_dawn ALIAS webgpu_dawn)
endif()

# Pakai ulang uamlib yang sudah dibangun di Tahap 4B
file(GLOB_RECURSE _hmmmgc_uam_a "${HMMMGC_ROOT}/stage4b_uam/build/*uam*.a")
if(_hmmmgc_uam_a AND NOT TARGET uamlib)
    list(GET _hmmmgc_uam_a 0 _uam_lib_path)
    add_library(uamlib STATIC IMPORTED GLOBAL)
    set_target_properties(uamlib PROPERTIES
        IMPORTED_LOCATION "${_uam_lib_path}"
        INTERFACE_INCLUDE_DIRECTORIES "${HMMMGC_ROOT}/deps/uam")
endif()
