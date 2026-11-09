# SPDX-License-Identifier: GPL-3.0-or-later

# Standalone present measurer. It is deliberately not part of engine/CMakeLists.txt and is not
# packaged. `just build-measure` builds and tests it, and `just install-measure` installs the layer.
cmake_minimum_required(VERSION 3.20)
project(fgvk-measure LANGUAGES CXX)

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
