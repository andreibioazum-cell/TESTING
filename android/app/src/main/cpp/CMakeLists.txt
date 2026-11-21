cmake_minimum_required(VERSION 3.22.1)
project(ObsidianCoreStudio LANGUAGES C CXX)
set(OBSIDIAN_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../..")

# Locate NDK glslc compiler (HINTS expects directories)
file(GLOB _GLSLC_NDK_DIRS
    "${ANDROID_NDK}/shader-tools/*"
    "${CMAKE_ANDROID_NDK}/shader-tools/*")
find_program(GLSLC_EXECUTABLE NAMES glslc HINTS ${_GLSLC_NDK_DIRS} REQUIRED)

set(SHADER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated_shaders")
file(MAKE_DIRECTORY "${SHADER_GEN_DIR}")

function(compile_spv_header SHADER_SRC VAR_NAME OUT_HEADER)
    set(_SPV "${SHADER_GEN_DIR}/${VAR_NAME}.spv")
    add_custom_command(
        OUTPUT "${_SPV}" "${OUT_HEADER}"
        COMMAND "${GLSLC_EXECUTABLE}" -O "${SHADER_SRC}" -o "${_SPV}"
        COMMAND "${CMAKE_COMMAND}"
            "-DINPUT_SPV=${_SPV}"
            "-DOUTPUT_HDR=${OUT_HEADER}"
            "-DVAR_NAME=${VAR_NAME}"
            -P "${CMAKE_CURRENT_LIST_DIR}/spv_to_header.cmake"
        DEPENDS "${SHADER_SRC}" "${CMAKE_CURRENT_LIST_DIR}/spv_to_header.cmake"
        COMMENT "Compiling SPIR-V ${VAR_NAME}"
        VERBATIM
    )
endfunction()

compile_spv_header("${CMAKE_CURRENT_LIST_DIR}/shaders/mesh3d.vert" "mesh3d_vert_spv" "${SHADER_GEN_DIR}/mesh3d_vert_spv.h")
compile_spv_header("${CMAKE_CURRENT_LIST_DIR}/shaders/mesh3d.frag" "mesh3d_frag_spv" "${SHADER_GEN_DIR}/mesh3d_frag_spv.h")
compile_spv_header("${CMAKE_CURRENT_LIST_DIR}/shaders/shadow.vert" "shadow_vert_spv" "${SHADER_GEN_DIR}/shadow_vert_spv.h")
compile_spv_header("${CMAKE_CURRENT_LIST_DIR}/shaders/ui2d.vert"   "ui2d_vert_spv"   "${SHADER_GEN_DIR}/ui2d_vert_spv.h")
compile_spv_header("${CMAKE_CURRENT_LIST_DIR}/shaders/ui2d.frag"   "ui2d_frag_spv"   "${SHADER_GEN_DIR}/ui2d_frag_spv.h")

set(SHADER_HEADERS
    "${SHADER_GEN_DIR}/mesh3d_vert_spv.h"
    "${SHADER_GEN_DIR}/mesh3d_frag_spv.h"
    "${SHADER_GEN_DIR}/shadow_vert_spv.h"
    "${SHADER_GEN_DIR}/ui2d_vert_spv.h"
    "${SHADER_GEN_DIR}/ui2d_frag_spv.h"
)

add_library(obsidian_compiler STATIC "${OBSIDIAN_ROOT}/src/obsidian_core.c")
target_include_directories(obsidian_compiler PRIVATE "${OBSIDIAN_ROOT}/src")
target_compile_definitions(obsidian_compiler PRIVATE OBSIDIAN_LIBRARY_ONLY OBSIDIAN_EMBEDDED_RUNTIME OBSIDIAN_RUNTIME_PATH="obsidronium_runtime.h")
set_target_properties(obsidian_compiler PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)

add_library(obsidian_native SHARED
    native.cpp
    vulkan_android.cpp
    studio_app.cpp
    editor.c
    ${SHADER_HEADERS}
)
target_include_directories(obsidian_native PRIVATE
    "${OBSIDIAN_ROOT}/src"
    "${CMAKE_CURRENT_LIST_DIR}"
    "${SHADER_GEN_DIR}"
)
target_compile_features(obsidian_native PRIVATE cxx_std_17)
target_link_libraries(obsidian_native PRIVATE obsidian_compiler android log vulkan m dl)
