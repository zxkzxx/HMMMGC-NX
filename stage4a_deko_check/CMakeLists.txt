cmake_minimum_required(VERSION 3.28)
project(stage4a_deko_check LANGUAGES C CXX)
get_filename_component(HMMMGC_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

add_subdirectory(${HMMMGC_ROOT}/deps/deko3d deko3d_build EXCLUDE_FROM_ALL)
nx_add_shader_program(test_imgui_vsh
    ${HMMMGC_ROOT}/deps/deko/shaders/imgui_vsh.glsl
    vert
    OUTPUT "${CMAKE_BINARY_DIR}/imgui_vsh.dksh")
