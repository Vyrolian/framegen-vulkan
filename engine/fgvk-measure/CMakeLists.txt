# SPDX-License-Identifier: GPL-3.0-or-later

# Standalone present measurer. It is deliberately not part of engine/CMakeLists.txt and is not
# packaged. `just build-measure` builds and tests it, and `just install-measure` installs the layer.
# The fgvk-pt-probe target is a separate standalone tool, also not packaged or installed (see below).
cmake_minimum_required(VERSION 3.20)
project(fgvk-measure LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)
find_path(FGVK_MEASURE_VULKAN_INCLUDE_DIR NAMES vulkan/vk_layer.h REQUIRED)

add_library(fgvk-measure SHARED
    src/events.cpp
    src/layer.cpp
    src/measure_log.cpp
    src/wayland_feedback.cpp
)
target_include_directories(fgvk-measure SYSTEM PRIVATE "${FGVK_MEASURE_VULKAN_INCLUDE_DIR}")
target_include_directories(fgvk-measure PRIVATE src)
target_compile_definitions(fgvk-measure PRIVATE
    _GNU_SOURCE
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WAYLAND_KHR
)
target_compile_options(fgvk-measure PRIVATE -Wall -Wextra)
set_target_properties(fgvk-measure PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_libraries(fgvk-measure PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)

enable_testing()
add_executable(fgvk-measure-unit-tests tests/measure_unit_tests.cpp src/events.cpp)
target_include_directories(fgvk-measure-unit-tests PRIVATE src)
target_compile_options(fgvk-measure-unit-tests PRIVATE -Wall -Wextra)
add_test(NAME measure-timestamp-conversion COMMAND fgvk-measure-unit-tests timestamp)
add_test(NAME measure-slot-ring COMMAND fgvk-measure-unit-tests slot-ring)
add_test(NAME measure-record-format COMMAND fgvk-measure-unit-tests record-format)

# Present-timing probe (probe/present_timing_probe.cpp): a standalone Wayland window that presents on a
# schedule to test VK_EXT_present_timing. The build never runs it. The wayland-protocols XML is compiled
# into the build directory, so no generated source is kept in the tree.
find_package(Python3 3.8 COMPONENTS Interpreter REQUIRED)
find_package(Vulkan REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FGVK_PROBE_WAYLAND REQUIRED IMPORTED_TARGET wayland-client)
pkg_get_variable(FGVK_PROBE_PROTOCOLS_DIR wayland-protocols pkgdatadir)
if(NOT FGVK_PROBE_PROTOCOLS_DIR)
    message(FATAL_ERROR "fgvk-pt-probe needs the wayland-protocols package (pkg-config: wayland-protocols)")
endif()
find_program(FGVK_PROBE_WAYLAND_SCANNER NAMES wayland-scanner REQUIRED)

set(FGVK_PROBE_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/probe-protocols")
file(MAKE_DIRECTORY "${FGVK_PROBE_GEN_DIR}")
set(FGVK_PROBE_GENERATED "")

function(fgvk_probe_protocol relative_xml base)
    set(xml "${FGVK_PROBE_PROTOCOLS_DIR}/${relative_xml}")
    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR "fgvk-pt-probe: missing protocol description ${xml}")
    endif()
    set(header "${FGVK_PROBE_GEN_DIR}/${base}-client-protocol.h")
    set(source "${FGVK_PROBE_GEN_DIR}/${base}-protocol.c")
    add_custom_command(
        OUTPUT "${header}"
        COMMAND "${FGVK_PROBE_WAYLAND_SCANNER}" client-header "${xml}" "${header}"
        DEPENDS "${xml}"
        VERBATIM)
    add_custom_command(
        OUTPUT "${source}"
        COMMAND "${FGVK_PROBE_WAYLAND_SCANNER}" private-code "${xml}" "${source}"
        DEPENDS "${xml}"
        VERBATIM)
    set(FGVK_PROBE_GENERATED ${FGVK_PROBE_GENERATED} "${header}" "${source}" PARENT_SCOPE)
endfunction()

fgvk_probe_protocol("stable/xdg-shell/xdg-shell.xml" xdg-shell)
fgvk_probe_protocol("stable/presentation-time/presentation-time.xml" presentation-time)

add_executable(fgvk-pt-probe probe/present_timing_probe.cpp ${FGVK_PROBE_GENERATED})
target_include_directories(fgvk-pt-probe PRIVATE "${FGVK_PROBE_GEN_DIR}")
target_compile_definitions(fgvk-pt-probe PRIVATE _GNU_SOURCE VK_USE_PLATFORM_WAYLAND_KHR)
target_compile_options(fgvk-pt-probe PRIVATE -Wall -Wextra)
target_link_libraries(fgvk-pt-probe PRIVATE Vulkan::Vulkan PkgConfig::FGVK_PROBE_WAYLAND Threads::Threads)

add_test(NAME measure-pt-probe-analyzer
    COMMAND Python3::Interpreter -I "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_pt_probe.py")
