cmake_minimum_required(VERSION 3.20)

# Standalone build for the unattended input-latency probe. It is deliberately not part of
# the engine's top-level CMake; engine/scripts/compare-latency.py drives it.
project(fgvk-latency-probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SDL3 REQUIRED)
find_package(Vulkan REQUIRED)
find_program(GLSLC_EXECUTABLE glslc REQUIRED)

# The fullscreen-triangle vertex stage has no tracked source of its own. It is written into
# the build tree at configure time and compiled next to probe.frag, so the probe directory
# keeps exactly the source files that the spec names.
set(PROBE_VERTEX_SOURCE [==[
#version 450

layout(location = 0) out vec2 v_uv;

void main() {
    vec2 uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    v_uv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
]==])
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/probe.vert"
    CONTENT "${PROBE_VERTEX_SOURCE}" @ONLY)

# Both SPIR-V modules land next to the binary; main.cpp loads them from /proc/self/exe.
set(PROBE_VERT_SPV "${CMAKE_CURRENT_BINARY_DIR}/probe.vert.spv")
set(PROBE_FRAG_SPV "${CMAKE_CURRENT_BINARY_DIR}/probe.frag.spv")

add_custom_command(
    OUTPUT "${PROBE_VERT_SPV}"
    COMMAND "${GLSLC_EXECUTABLE}" -fshader-stage=vert "${CMAKE_CURRENT_BINARY_DIR}/probe.vert"
            -o "${PROBE_VERT_SPV}"
    DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/probe.vert"
    COMMENT "Compiling latency probe vertex shader"
    VERBATIM)

add_custom_command(
    OUTPUT "${PROBE_FRAG_SPV}"
    COMMAND "${GLSLC_EXECUTABLE}" -fshader-stage=frag "${CMAKE_CURRENT_SOURCE_DIR}/probe.frag"
            -o "${PROBE_FRAG_SPV}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/probe.frag"
    COMMENT "Compiling latency probe fragment shader"
    VERBATIM)

add_custom_target(fgvk-latency-probe-shaders ALL DEPENDS "${PROBE_VERT_SPV}" "${PROBE_FRAG_SPV}")

add_executable(fgvk-latency-probe main.cpp)
add_dependencies(fgvk-latency-probe fgvk-latency-probe-shaders)
set_target_properties(fgvk-latency-probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_options(fgvk-latency-probe PRIVATE -Wall -Wextra)
target_link_libraries(fgvk-latency-probe PRIVATE SDL3::SDL3 Vulkan::Vulkan)
