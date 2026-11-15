cmake_minimum_required(VERSION 3.10)
project(fgvk-render LANGUAGES CXX)
include(GNUInstallDirs)
include(CTest)

set(FGVK_REPOSITORY_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE")
set(FGVK_REPOSITORY_COPYRIGHT "${CMAKE_CURRENT_SOURCE_DIR}/../COPYRIGHT.md")
set(FGVK_THIRD_PARTY_NOTICES "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_NOTICES.md")
set(FGVK_ASSET_PROVENANCE "${CMAKE_CURRENT_SOURCE_DIR}/../ASSET_PROVENANCE.md")
if(NOT EXISTS "${FGVK_REPOSITORY_LICENSE}")
    message(FATAL_ERROR "The monorepo license is missing: ${FGVK_REPOSITORY_LICENSE}")
endif()
if(NOT EXISTS "${FGVK_REPOSITORY_COPYRIGHT}")
    message(FATAL_ERROR "The monorepo copyright notices are missing: ${FGVK_REPOSITORY_COPYRIGHT}")
endif()
if(NOT EXISTS "${FGVK_THIRD_PARTY_NOTICES}")
    message(FATAL_ERROR "The monorepo third-party notices are missing: ${FGVK_THIRD_PARTY_NOTICES}")
endif()
if(NOT EXISTS "${FGVK_ASSET_PROVENANCE}")
    message(FATAL_ERROR "The monorepo asset provenance record is missing: ${FGVK_ASSET_PROVENANCE}")
endif()
install(FILES "${FGVK_REPOSITORY_LICENSE}" "${FGVK_REPOSITORY_COPYRIGHT}" "${FGVK_THIRD_PARTY_NOTICES}" "${FGVK_ASSET_PROVENANCE}"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/framegen-vulkan")
install(PROGRAMS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fgvk-installer"
    DESTINATION "${CMAKE_INSTALL_BINDIR}")

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" FGVK_VERSION LIMIT_COUNT 1)
string(STRIP "${FGVK_VERSION}" FGVK_VERSION)
if(FGVK_VERSION STREQUAL "")
    message(FATAL_ERROR "VERSION must contain a build identity")
endif()

# Released source archives are identified by VERSION. Development checkouts
# also include the component commit and a short digest of tracked changes and
# untracked paths so two local binaries from the same release line cannot
# produce indistinguishable diagnostics. Keep this configure-time only: the
# Vulkan presentation path must never inspect Git or hash files at runtime.
set(FGVK_BUILD_FINGERPRINT "${FGVK_VERSION}")
find_program(FGVK_GIT_EXECUTABLE NAMES git)
if(FGVK_GIT_EXECUTABLE)
    execute_process(
        COMMAND "${FGVK_GIT_EXECUTABLE}" log -1 --format=%H -- .
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE FGVK_GIT_COMMIT_RESULT
        OUTPUT_VARIABLE FGVK_GIT_COMMIT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(FGVK_GIT_COMMIT_RESULT EQUAL 0 AND NOT FGVK_GIT_COMMIT STREQUAL "")
        string(SUBSTRING "${FGVK_GIT_COMMIT}" 0 12 FGVK_GIT_SHORT_COMMIT)
        execute_process(
            COMMAND "${FGVK_GIT_EXECUTABLE}" diff --binary HEAD -- .
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE FGVK_GIT_DIFF
            ERROR_QUIET)
        execute_process(
            COMMAND "${FGVK_GIT_EXECUTABLE}" status --porcelain
                --untracked-files=normal -- .
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            OUTPUT_VARIABLE FGVK_GIT_STATUS
            ERROR_QUIET)
        set(FGVK_BUILD_FINGERPRINT "${FGVK_GIT_SHORT_COMMIT}")
        if(NOT FGVK_GIT_STATUS STREQUAL "")
            string(SHA256 FGVK_GIT_DIRTY_DIGEST
                "${FGVK_GIT_DIFF}\n${FGVK_GIT_STATUS}")
            string(SUBSTRING "${FGVK_GIT_DIRTY_DIGEST}" 0 8
                FGVK_GIT_DIRTY_SHORT_DIGEST)
            string(APPEND FGVK_BUILD_FINGERPRINT
                ".dirty.${FGVK_GIT_DIRTY_SHORT_DIGEST}")
        endif()
    endif()
endif()

# === READ HERE FOR BUILD OPTIONS ===
option(FGVK_BUILD_VK_LAYER     "Build the Vulkan layer"            ON)
option(FGVK_BUILD_CLI          "Build the command line interface"  ON)
option(FGVK_INSTALL_DEVELOP    "Install development libraries and headers"      OFF)
option(FGVK_ENABLE_SANITIZERS  "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(FGVK_EXPERIMENTAL_LSFG_FP16 "Convert FP32-only LSFG models when the existing FP16 setting is enabled (quality unqualified)" OFF)
set(FGVK_LSFG_FP16_TOOLS_SOURCE "" CACHE PATH "Pinned SPIRV-Tools source prepared by scripts/prepare-lsfg-fp16-tools.sh")
if(FGVK_EXPERIMENTAL_LSFG_FP16)
    string(APPEND FGVK_BUILD_FINGERPRINT ".experimental-lsfg-fp16")
endif()
option(FGVK_REQUIRE_NATIVE_PACKAGE_HEADERS "Require the shared package Vulkan header baseline" OFF)
set(FGVK_VULKAN_HEADERS_INCLUDE_DIR "" CACHE PATH "Optional Vulkan-Headers include directory for direct builds")
if(FGVK_VULKAN_HEADERS_INCLUDE_DIR)
    if(NOT EXISTS "${FGVK_VULKAN_HEADERS_INCLUDE_DIR}/vulkan/vulkan_core.h")
        message(FATAL_ERROR
            "FGVK_VULKAN_HEADERS_INCLUDE_DIR does not contain Vulkan-Headers: "
            "${FGVK_VULKAN_HEADERS_INCLUDE_DIR}")
    endif()
    include_directories(BEFORE SYSTEM "${FGVK_VULKAN_HEADERS_INCLUDE_DIR}")
endif()
set(FGVK_LAYER_LIBRARY_PATH libfgvk-render.so CACHE STRING "Change where Vulkan searches for the layer library")
set(FGVK_LAYER_MANIFEST_SUFFIX "" CACHE STRING "Suffix for the installed Vulkan layer manifest filename")
option(FGVK_TESTING_RENDERDOC  "Enable RenderDoc integration for testing purposes" OFF)
# === READ HERE FOR BUILD OPTIONS ===

if(FGVK_BUILD_VK_LAYER)
    # The launch helper is private: `fgvk <command>` executes it, so it is not on PATH.
    install(PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fgvk-launch"
        DESTINATION "${CMAKE_INSTALL_LIBEXECDIR}/framegen-vulkan")
endif()

if(BUILD_TESTING AND UNIX)
    find_program(FGVK_PYTHON3_EXECUTABLE NAMES python3)
    if(NOT FGVK_PYTHON3_EXECUTABLE)
        message(FATAL_ERROR "Python 3 is required for Renderer source-contract tests")
    endif()

    add_test(NAME standalone-launcher
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-fgvk-launch.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fgvk-launch")
    add_test(NAME standalone-mangohud
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-fgvk-launch-mangohud.py")
    set_tests_properties(standalone-mangohud PROPERTIES LABELS "portable-policy")
    set_tests_properties(standalone-launcher PROPERTIES
        LABELS "portable-policy")

    add_test(NAME steamos-build-tools
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-install-steamos-build-tools.py")
    set_tests_properties(steamos-build-tools PROPERTIES
        LABELS "portable-policy")

    add_test(NAME standalone-installer
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-fgvk-installer.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fgvk-installer")
    set_tests_properties(standalone-installer PROPERTIES
        LABELS "portable-policy")

    add_test(NAME standalone-flatpak-installer
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-fgvk-flatpak-installer.sh"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/fgvk-install-flatpaks")
    set_tests_properties(standalone-flatpak-installer PROPERTIES
        LABELS "portable-policy")

    add_test(NAME flatpak-runtime-freshness
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/generate-flatpak-runtimes.py"
            --check)
    set_tests_properties(flatpak-runtime-freshness PROPERTIES LABELS "portable-policy")

    add_test(NAME flatpak-vulkan-headers-freshness
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-flatpak-vulkan-headers.py"
            --check)
    add_test(NAME flatpak-vulkan-headers-contract
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/test-flatpak-vulkan-headers.py")
    set_tests_properties(flatpak-vulkan-headers-freshness
        flatpak-vulkan-headers-contract PROPERTIES LABELS "portable-policy")

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_test(NAME arch-package-release-pin
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/dist/arch/check-release-pin.sh")
        add_test(NAME arch-package-contract
            COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/dist/arch/test-package-contract.sh")
        set_tests_properties(arch-package-release-pin
            arch-package-contract PROPERTIES LABELS "portable-policy")
    endif()

    add_test(NAME embedded-color-conversion-spirv-freshness
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate-color-conversion-spirv.py"
            --check)
    set_tests_properties(embedded-color-conversion-spirv-freshness PROPERTIES
        LABELS "portable-policy")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_SKIP_RPATH ON)

if(FGVK_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR
            "FGVK_ENABLE_SANITIZERS requires GCC, Clang, or AppleClang")
    endif()
    add_compile_options(
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all)
    set(CMAKE_EXE_LINKER_FLAGS
        "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    set(CMAKE_SHARED_LINKER_FLAGS
        "${CMAKE_SHARED_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

    if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
        message(STATUS "Building with clang-specific diagnostics")

        set(CMAKE_CXX_CLANG_TIDY clang-tidy) # See .clang-tidy files

        add_compile_options(
            # By default, enable all warnings
            -Weverything
            # Some warnings are incompatible with each other
            -Wno-pre-c++20-compat-pedantic
            -Wno-c++98-compat-pedantic
            -Wno-switch-default
            # Then there's code-style things I don't care about
            -Wno-missing-designated-field-initializers
            -Wno-shadow
            # And functional warning I don't care about either
            -Wno-cast-function-type-strict
            -Wno-padded
        )
    endif()
endif()

if(FGVK_TESTING_RENDERDOC)
    add_compile_definitions(FGVK_TESTING_RENDERDOC)
endif()

if(FGVK_BUILD_VK_LAYER OR FGVK_BUILD_CLI OR
        FGVK_INSTALL_DEVELOP OR BUILD_TESTING)
    add_subdirectory(fgvk-common)
endif()
if(FGVK_BUILD_VK_LAYER OR FGVK_BUILD_CLI OR FGVK_INSTALL_DEVELOP)
    add_subdirectory(fgvk-backend)
endif()
if(FGVK_BUILD_VK_LAYER OR FGVK_BUILD_CLI OR BUILD_TESTING)
    add_subdirectory(fgvk-render)
endif()
if(FGVK_BUILD_CLI)
    add_subdirectory(fgvk-cli)
endif()
