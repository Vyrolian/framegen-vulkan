# framegen-vulkan reflexlog: testing-only pass-through capture layer.
# Standalone project. It is not part of engine/CMakeLists.txt and is not packaged.
cmake_minimum_required(VERSION 3.20)
project(reflexlog LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "framegen-vulkan: reflexlog builds only as a 64-bit layer")
endif()

set(REFLEXLOG_VULKAN_INCLUDE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../build/vulkan-headers/v1.4.365/include"
    CACHE PATH "Directory that contains vulkan/vulkan.h and vulkan/vk_layer.h")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED)

add_library(VkLayer_reflexlog SHARED reflexlog.cpp)
target_include_directories(VkLayer_reflexlog PRIVATE "${REFLEXLOG_VULKAN_INCLUDE_DIR}")
target_compile_options(VkLayer_reflexlog PRIVATE -Wall -Wextra)
set_target_properties(VkLayer_reflexlog PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(VkLayer_reflexlog PRIVATE Threads::Threads)

# Export only the three layer entry points; everything else stays local.
set(REFLEXLOG_VERSION_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/reflexlog.map")
file(WRITE "${REFLEXLOG_VERSION_SCRIPT}"
  "{\n  global:\n    vkNegotiateLoaderLayerInterfaceVersion;\n    vkGetInstanceProcAddr;\n    vkGetDeviceProcAddr;\n  local:\n    *;\n};\n")
set_target_properties(VkLayer_reflexlog PROPERTIES LINK_DEPENDS "${REFLEXLOG_VERSION_SCRIPT}")
target_link_options(VkLayer_reflexlog PRIVATE
  "-Wl,--version-script=${REFLEXLOG_VERSION_SCRIPT}"
  "-Wl,--no-undefined")
