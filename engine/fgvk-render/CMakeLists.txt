set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/gamescope_hdr_feedback.cpp"
    "src/gamescope_scaling_surface.cpp"
    "src/instance.cpp"
    "src/present_diagnostics.cpp"
    "src/frame_content_capture.cpp"
    "src/wayland_presentation_feedback.cpp"
    "src/runtime_health.cpp"
    "src/runtime_status.cpp"
    "src/swapchain/create.cpp"
    "src/swapchain/internal_images.cpp"
    "src/swapchain/present.cpp"
    "src/swapchain/present/admission.cpp"
    "src/swapchain/present/generated_frames.cpp"
    "src/swapchain/present/real_frames.cpp"
    "src/swapchain/present/retirement.cpp"
    "src/swapchain/profile.cpp"
    "src/swapchain/resources.cpp"
    "src/swapchain/status.cpp")

set(ADAPTIVE_SOURCE "src/adaptive_scheduler.cpp")
set(SPATIAL_SCALER_SOURCE "src/spatial_scaler.cpp")
set(COLOR_PIPELINE_SOURCE "src/color_pipeline.cpp")

# Keep explicit target membership while detecting a newly added implementation
# that was forgotten in CMake. The checker runs for the default and layer
# targets, so adding a source under src/ fails even in an existing build tree.
set(RENDERER_PRODUCTION_SOURCES
    ${LAYER_SOURCES}
    ${ADAPTIVE_SOURCE}
    ${SPATIAL_SCALER_SOURCE}
    ${COLOR_PIPELINE_SOURCE})
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/check-source-inventory.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/check-source-inventory.cmake"
    @ONLY)
add_custom_target(fgvk-renderer-source-inventory
    COMMAND "${CMAKE_COMMAND}" -P
        "${CMAKE_CURRENT_BINARY_DIR}/check-source-inventory.cmake"
    COMMENT "Checking framegen-vulkan source inventory"
    VERBATIM)

add_library(fgvk-adaptive STATIC
    ${ADAPTIVE_SOURCE})
add_dependencies(fgvk-adaptive fgvk-renderer-source-inventory)
target_include_directories(fgvk-adaptive
    PUBLIC "src"
    PUBLIC "../fgvk-common/include")
set_target_properties(fgvk-adaptive PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)

if(FGVK_BUILD_VK_LAYER OR FGVK_BUILD_CLI)
    add_library(fgvk-spatial-scaler STATIC
        ${SPATIAL_SCALER_SOURCE})
    add_dependencies(fgvk-spatial-scaler fgvk-renderer-source-inventory)
    target_include_directories(fgvk-spatial-scaler
        PUBLIC "src")
    target_link_libraries(fgvk-spatial-scaler
        PUBLIC fgvk-common
        PUBLIC fgvk-backend)
    set_target_properties(fgvk-spatial-scaler PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
endif()

if(FGVK_BUILD_VK_LAYER OR BUILD_TESTING)
    find_package(Threads REQUIRED)
endif()

if(FGVK_BUILD_VK_LAYER)
    if(FGVK_REQUIRE_NATIVE_PACKAGE_HEADERS)
        # Run for each architecture even when tests are disabled. Compile the
        # actual filtering header with this build's compiler/include flags so
        # an older SDK cannot silently remove required presentation handling.
        include(CheckCXXSourceCompiles)
        function(fgvk_check_native_package_headers)
            set(revision_file "${CMAKE_CURRENT_SOURCE_DIR}/../vulkan-headers-revision.txt")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${revision_file}")
            file(READ "${revision_file}" revision)
            string(STRIP "${revision}" revision)
            if(NOT revision MATCHES "^(vulkan-sdk-|v)([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
                message(FATAL_ERROR
                    "vulkan-headers-revision.txt must contain a Vulkan-Headers "
                    "SDK branch (vulkan-sdk-X.Y.Z) or release tag (vX.Y.Z).")
            endif()
            set(header_version "${CMAKE_MATCH_2}.${CMAKE_MATCH_3}.${CMAKE_MATCH_4}")
            set(header_version_arguments "${CMAKE_MATCH_2}, ${CMAKE_MATCH_3}, ${CMAKE_MATCH_4}")
            set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
            if(FGVK_VULKAN_HEADERS_INCLUDE_DIR)
                set(CMAKE_REQUIRED_INCLUDES "${FGVK_VULKAN_HEADERS_INCLUDE_DIR}")
            endif()
            unset(FGVK_NATIVE_PACKAGE_HEADERS_SUPPORTED CACHE)
            check_cxx_source_compiles("
                #include \"${CMAKE_CURRENT_SOURCE_DIR}/src/pnext_chain.hpp\"
                #ifndef VK_KHR_present_id2
                #error framegen-vulkan packages require VK_KHR_present_id2
                #endif
                #ifndef VK_EXT_present_timing
                #error framegen-vulkan packages require VK_EXT_present_timing
                #endif
                static_assert(VK_HEADER_VERSION_COMPLETE >=
                    VK_MAKE_API_VERSION(0, ${header_version_arguments}));
                static_assert(sizeof(VkPresentId2KHR) > 0);
                static_assert(sizeof(VkPresentTimingsInfoEXT) > 0);
                int main() { return 0; }
            " FGVK_NATIVE_PACKAGE_HEADERS_SUPPORTED)
            if(NOT FGVK_NATIVE_PACKAGE_HEADERS_SUPPORTED)
                message(FATAL_ERROR
                    "framegen-vulkan packages require Vulkan headers "
                    "${header_version} or newer with VK_KHR_present_id2 and "
                    "VK_EXT_present_timing. Use the pinned headers through "
                    "scripts/build-steamos-dev.sh, "
                    "scripts/package-local.sh (fetches the pinned headers), or "
                    "scripts/package-flatpaks.sh; "
                    "do not bypass this check. See the CMake compile log.")
            endif()
        endfunction()
        fgvk_check_native_package_headers()
    endif()

    add_library(fgvk-color STATIC
        ${COLOR_PIPELINE_SOURCE})
    add_dependencies(fgvk-color fgvk-renderer-source-inventory)
    target_include_directories(fgvk-color
        PUBLIC "src")
    target_link_libraries(fgvk-color
        PUBLIC fgvk-backend)
    set_target_properties(fgvk-color PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)

    function(fgvk_configure_renderer_layer target)
        target_link_libraries(${target}
            PRIVATE fgvk-adaptive
            PRIVATE fgvk-color
            PRIVATE fgvk-spatial-scaler
            PRIVATE Threads::Threads
            PRIVATE ${CMAKE_DL_LIBS}
            PUBLIC fgvk-common
            PUBLIC fgvk-backend)

        target_compile_options(${target} PRIVATE
            -Wno-unknown-warning-option
            -Wno-unsafe-buffer-usage) # Array indexing

        target_compile_definitions(${target} PRIVATE
            FGVK_BUILD_VERSION="${FGVK_VERSION}"
            FGVK_BUILD_FINGERPRINT="${FGVK_BUILD_FINGERPRINT}")

        set_target_properties(${target} PROPERTIES
            CXX_VISIBILITY_PRESET hidden)
    endfunction()

    add_library(fgvk-render SHARED ${LAYER_SOURCES})
    add_dependencies(fgvk-render fgvk-renderer-source-inventory)
    fgvk_configure_renderer_layer(fgvk-render)

    # Frame generation and scaling occupy opposite sides of Gamescope WSI.
    # Build a second isolated copy so the loader can form the only chain that
    # preserves both contracts: FG -> Gamescope WSI -> spatial scaling.
    add_library(fgvk-render-scaling SHARED ${LAYER_SOURCES})
    add_dependencies(fgvk-render-scaling fgvk-renderer-source-inventory)
    target_compile_definitions(fgvk-render-scaling PRIVATE
        FGVK_LAYER_ROLE_SPATIAL_SCALING=1)
    fgvk_configure_renderer_layer(fgvk-render-scaling)

    # The upper role consumes a same-process relay exported by the separately
    # compiled lower role. Keep targeted developer builds coherent: rebuilding
    # the public renderer target must never leave the spatial DSO stale.
    add_dependencies(fgvk-render fgvk-render-scaling)
endif()

if(BUILD_TESTING)
    add_executable(fgvk-spatial-command-recording-tests "tests/spatial_command_recording_tests.cpp")
    target_include_directories(fgvk-spatial-command-recording-tests PRIVATE src)
    add_test(NAME spatial-command-recording COMMAND fgvk-spatial-command-recording-tests)
    add_test(NAME renderer-source-inventory
        COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/check-source-inventory.cmake")
    set_tests_properties(renderer-source-inventory PROPERTIES
        LABELS "portable-policy")

    # Keep portable build and CTest membership together so the shell runner
    # cannot drift when another policy test is added.
    add_custom_target(fgvk-portable-policy-test-binaries)
    add_dependencies(fgvk-portable-policy-test-binaries fgvk-power-source-monitor-tests)
    function(fgvk_register_portable_policy_test target test_name)
        add_dependencies(fgvk-portable-policy-test-binaries ${target})
        set_tests_properties(${test_name} PROPERTIES
            LABELS "portable-policy")
    endfunction()

    fgvk_register_portable_policy_test(fgvk-spatial-command-recording-tests spatial-command-recording)

    add_dependencies(fgvk-portable-policy-test-binaries
        fgvk-external-fd-tests fgvk-descriptor-pool-tests fgvk-shader-pipeline-tests
        fgvk-command-buffer-submit-storage-tests)

    add_library(fgvk-launch-preload-fixture SHARED
        "../scripts/test-launch-preload.cpp")
    target_compile_definitions(fgvk-launch-preload-fixture PRIVATE FGVK_TEST_RETAINED)
    add_executable(fgvk-steam-overlay-diagnostics-tests
        "tests/steam_overlay_libraries_tests.cpp" "src/present_diagnostics.cpp")
    target_include_directories(fgvk-steam-overlay-diagnostics-tests PRIVATE "src")
    target_link_libraries(fgvk-steam-overlay-diagnostics-tests
        PRIVATE fgvk-adaptive ${CMAKE_DL_LIBS})
    add_dependencies(fgvk-steam-overlay-diagnostics-tests fgvk-launch-preload-fixture)
    add_test(NAME steam-overlay-library-diagnostics
        COMMAND fgvk-steam-overlay-diagnostics-tests $<TARGET_FILE:fgvk-launch-preload-fixture>)
    fgvk_register_portable_policy_test(fgvk-steam-overlay-diagnostics-tests steam-overlay-library-diagnostics)

    add_executable(fgvk-pacing-profile-tests
        "tests/pacing_profile_tests.cpp" "src/present_diagnostics.cpp")
    target_include_directories(fgvk-pacing-profile-tests PRIVATE "src")
    target_link_libraries(fgvk-pacing-profile-tests PRIVATE fgvk-adaptive ${CMAKE_DL_LIBS})
    add_test(NAME pacing-profile COMMAND fgvk-pacing-profile-tests)
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests pacing-profile)
    add_executable(fgvk-native-copy-profile-tests
        "tests/native_copy_profile_tests.cpp" "src/present_diagnostics.cpp")
    target_include_directories(fgvk-native-copy-profile-tests PRIVATE "src")
    target_link_libraries(fgvk-native-copy-profile-tests PRIVATE fgvk-adaptive ${CMAKE_DL_LIBS})
    add_test(NAME native-copy-profile COMMAND fgvk-native-copy-profile-tests)
    fgvk_register_portable_policy_test(fgvk-native-copy-profile-tests native-copy-profile)
    add_test(NAME pacing-profile-report
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pacing_profile_report.py")
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests pacing-profile-report)
    add_test(NAME framegen-comparison-profiler
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_framegen_profiler.py")
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests framegen-comparison-profiler)
    add_test(NAME latency-comparison
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_latency_compare.py")
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests latency-comparison)
    add_executable(fgvk-pacing-profile-benchmark
        "tests/pacing_profile_benchmark.cpp" "src/present_diagnostics.cpp")
    target_include_directories(fgvk-pacing-profile-benchmark PRIVATE "src")
    target_link_libraries(fgvk-pacing-profile-benchmark PRIVATE fgvk-adaptive ${CMAKE_DL_LIBS})

    add_executable(fgvk-runtime-health-tests
        "tests/runtime_health_tests.cpp" "src/runtime_health.cpp")
    target_link_libraries(fgvk-runtime-health-tests PRIVATE fgvk-common Threads::Threads)
    target_include_directories(fgvk-runtime-health-tests PRIVATE "src")
    add_test(NAME runtime-health COMMAND fgvk-runtime-health-tests)
    fgvk_register_portable_policy_test(fgvk-runtime-health-tests runtime-health)

    add_executable(fgvk-gpu-headroom-tests
        "tests/gpu_headroom_tests.cpp" "src/runtime_health.cpp")
    target_link_libraries(fgvk-gpu-headroom-tests PRIVATE fgvk-common Threads::Threads)
    target_include_directories(fgvk-gpu-headroom-tests PRIVATE "src")
    add_test(NAME gpu-headroom COMMAND fgvk-gpu-headroom-tests)
    fgvk_register_portable_policy_test(fgvk-gpu-headroom-tests gpu-headroom)

    add_executable(fgvk-device-selection-tests
        "tests/device_selection_tests.cpp")
    target_include_directories(fgvk-device-selection-tests
        PRIVATE "src")
    add_test(NAME device-selection
        COMMAND fgvk-device-selection-tests)
    fgvk_register_portable_policy_test(
        fgvk-device-selection-tests device-selection)

    add_executable(fgvk-profile-update-tests
        "tests/profile_update_tests.cpp")
    target_include_directories(fgvk-profile-update-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    add_test(NAME profile-update
        COMMAND fgvk-profile-update-tests)
    fgvk_register_portable_policy_test(
        fgvk-profile-update-tests profile-update)

    add_executable(fgvk-power-profile-update-tests
        "tests/power_profile_update_tests.cpp")
    target_link_libraries(fgvk-power-profile-update-tests PRIVATE fgvk-common)
    target_include_directories(fgvk-power-profile-update-tests PRIVATE "src")
    add_test(NAME power-profile-update COMMAND fgvk-power-profile-update-tests)
    fgvk_register_portable_policy_test(
        fgvk-power-profile-update-tests power-profile-update)

    add_executable(fgvk-spatial-scaling-policy-tests
        "tests/spatial_scaling_policy_tests.cpp")
    target_include_directories(fgvk-spatial-scaling-policy-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    add_test(NAME spatial-scaling-policy
        COMMAND fgvk-spatial-scaling-policy-tests)
    fgvk_register_portable_policy_test(
        fgvk-spatial-scaling-policy-tests spatial-scaling-policy)

    add_executable(fgvk-swapchain-retirement-tests
        "tests/swapchain_retirement_tests.cpp")
    target_link_libraries(fgvk-swapchain-retirement-tests
        PRIVATE Threads::Threads)
    target_include_directories(fgvk-swapchain-retirement-tests
        PRIVATE "src")
    add_test(NAME swapchain-retirement
        COMMAND fgvk-swapchain-retirement-tests)
    fgvk_register_portable_policy_test(
        fgvk-swapchain-retirement-tests swapchain-retirement)

    add_executable(fgvk-present-timing-tests "tests/present_timing_tests.cpp")
    target_include_directories(fgvk-present-timing-tests PRIVATE "src")
    add_test(NAME present-timing COMMAND fgvk-present-timing-tests)
    fgvk_register_portable_policy_test(fgvk-present-timing-tests present-timing)

    add_executable(fgvk-generated-frame-plan-tests
        "tests/generated_frame_plan_tests.cpp")
    target_include_directories(fgvk-generated-frame-plan-tests
        PRIVATE "src")
    add_test(NAME generated-frame-plan
        COMMAND fgvk-generated-frame-plan-tests)
    fgvk_register_portable_policy_test(
        fgvk-generated-frame-plan-tests generated-frame-plan)

    add_executable(fgvk-runtime-transition-tests
        "tests/runtime_transition_tests.cpp")
    target_include_directories(fgvk-runtime-transition-tests
        PRIVATE "src")
    add_test(NAME runtime-transition
        COMMAND fgvk-runtime-transition-tests)
    fgvk_register_portable_policy_test(
        fgvk-runtime-transition-tests runtime-transition)

    add_executable(fgvk-runtime-status-tests
        "tests/runtime_status_tests.cpp"
        "src/runtime_status.cpp")
    target_include_directories(fgvk-runtime-status-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    target_link_libraries(fgvk-runtime-status-tests
        PRIVATE fgvk-common)
    add_test(NAME runtime-status
        COMMAND fgvk-runtime-status-tests)
    fgvk_register_portable_policy_test(
        fgvk-runtime-status-tests runtime-status)

    add_executable(fgvk-layer-role-tests
        "tests/layer_role_tests.cpp")
    target_include_directories(fgvk-layer-role-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    add_test(NAME layer-role
        COMMAND fgvk-layer-role-tests)
    fgvk_register_portable_policy_test(
        fgvk-layer-role-tests layer-role)

    add_executable(fgvk-spatial-layer-role-tests
        "tests/layer_role_tests.cpp")
    target_compile_definitions(fgvk-spatial-layer-role-tests PRIVATE
        FGVK_LAYER_ROLE_SPATIAL_SCALING=1)
    target_include_directories(fgvk-spatial-layer-role-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    add_test(NAME spatial-layer-role
        COMMAND fgvk-spatial-layer-role-tests)
    fgvk_register_portable_policy_test(
        fgvk-spatial-layer-role-tests spatial-layer-role)

    add_executable(fgvk-presentation-policy-tests
        "tests/presentation_policy_tests.cpp")
    target_include_directories(fgvk-presentation-policy-tests
        PRIVATE "src")
    add_test(NAME presentation-policy
        COMMAND fgvk-presentation-policy-tests)
    fgvk_register_portable_policy_test(
        fgvk-presentation-policy-tests presentation-policy)

    add_executable(fgvk-deferred-present-tests "tests/deferred_present_tests.cpp")
    target_include_directories(fgvk-deferred-present-tests PRIVATE "src")
    target_link_libraries(fgvk-deferred-present-tests PRIVATE Threads::Threads)
    add_test(NAME deferred-present COMMAND fgvk-deferred-present-tests)
    fgvk_register_portable_policy_test(fgvk-deferred-present-tests deferred-present)

    add_executable(fgvk-release-gate-tests "tests/release_gate_tests.cpp")
    target_include_directories(fgvk-release-gate-tests PRIVATE "src")
    target_link_libraries(fgvk-release-gate-tests PRIVATE Threads::Threads)
    add_test(NAME release-gate COMMAND fgvk-release-gate-tests)
    fgvk_register_portable_policy_test(fgvk-release-gate-tests release-gate)

    add_executable(fgvk-render-slot-ring-tests "tests/render_slot_ring_tests.cpp")
    target_include_directories(fgvk-render-slot-ring-tests PRIVATE "src")
    add_test(NAME render-slot-ring COMMAND fgvk-render-slot-ring-tests)
    fgvk_register_portable_policy_test(fgvk-render-slot-ring-tests render-slot-ring)

    # Internal application images: pure gate and free-image ring tests. The
    # ring test also runs a short two-thread acquire/present exchange.
    add_executable(fgvk-internal-images-tests "tests/internal_images_tests.cpp")
    target_include_directories(fgvk-internal-images-tests PRIVATE "src")
    target_link_libraries(fgvk-internal-images-tests PRIVATE Threads::Threads)
    add_test(NAME internal-images COMMAND fgvk-internal-images-tests)
    fgvk_register_portable_policy_test(fgvk-internal-images-tests internal-images)

    # fgvk-backend is configured only for layer, CLI or develop builds. Its
    # fence ring is header-only, so the portable policy set builds this backend
    # test from the backend tree and keeps it in the same label.
    add_executable(fgvk-backend-fence-ring-tests "../fgvk-backend/tests/fence_ring_tests.cpp")
    target_include_directories(fgvk-backend-fence-ring-tests PRIVATE "../fgvk-backend/src")
    add_test(NAME backend-fence-ring COMMAND fgvk-backend-fence-ring-tests)
    fgvk_register_portable_policy_test(fgvk-backend-fence-ring-tests backend-fence-ring)

    add_executable(fgvk-uncapped-auto-cap-tests "tests/uncapped_auto_cap_tests.cpp")
    target_include_directories(fgvk-uncapped-auto-cap-tests PRIVATE "src")
    add_test(NAME uncapped-auto-cap COMMAND fgvk-uncapped-auto-cap-tests)
    fgvk_register_portable_policy_test(fgvk-uncapped-auto-cap-tests uncapped-auto-cap)

    add_executable(fgvk-present-feedback-probe-tests "tests/present_feedback_probe_tests.cpp")
    target_include_directories(fgvk-present-feedback-probe-tests PRIVATE "src")
    add_test(NAME present-feedback-probe COMMAND fgvk-present-feedback-probe-tests)
    fgvk_register_portable_policy_test(fgvk-present-feedback-probe-tests present-feedback-probe)

    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_library(fgvk-wayland-presentation-fixture SHARED "tests/wayland_presentation_client_fixture.cpp")
        target_include_directories(fgvk-wayland-presentation-fixture PRIVATE src)
        set_target_properties(fgvk-wayland-presentation-fixture PROPERTIES
            OUTPUT_NAME "wayland-client" SOVERSION 0
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/wayland-presentation-fixture")
        add_executable(fgvk-wayland-presentation-tests
            "tests/wayland_presentation_feedback_tests.cpp" "src/wayland_presentation_feedback.cpp" "src/present_diagnostics.cpp")
        target_include_directories(fgvk-wayland-presentation-tests PRIVATE src)
        target_link_libraries(fgvk-wayland-presentation-tests PRIVATE fgvk-wayland-presentation-fixture fgvk-adaptive Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME wayland-presentation-feedback COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-wayland-presentation-fixture>" $<TARGET_FILE:fgvk-wayland-presentation-tests>)
        fgvk_register_portable_policy_test(fgvk-wayland-presentation-tests wayland-presentation-feedback)

        add_library(fgvk-feedback-client-fixture SHARED
            "tests/gamescope_feedback_client_fixture.cpp")
        set_target_properties(fgvk-feedback-client-fixture PROPERTIES
            OUTPUT_NAME "X11" SOVERSION 6
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/feedback-client-fixture")
        add_executable(fgvk-gamescope-feedback-reader-tests
            "tests/gamescope_feedback_reader_tests.cpp"
            "src/gamescope_hdr_feedback.cpp")
        target_include_directories(fgvk-gamescope-feedback-reader-tests
            PRIVATE "src" "../fgvk-common/include")
        target_link_libraries(fgvk-gamescope-feedback-reader-tests
            PRIVATE fgvk-feedback-client-fixture Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME gamescope-feedback-reader
            COMMAND ${CMAKE_COMMAND} -E env
                "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-feedback-client-fixture>"
                $<TARGET_FILE:fgvk-gamescope-feedback-reader-tests>)
        fgvk_register_portable_policy_test(
            fgvk-gamescope-feedback-reader-tests gamescope-feedback-reader)

        add_library(fgvk-surface-client-fixture SHARED
            "tests/gamescope_surface_client_fixture.cpp")
        target_link_libraries(fgvk-surface-client-fixture PRIVATE Threads::Threads)
        set_target_properties(fgvk-surface-client-fixture PROPERTIES
            OUTPUT_NAME "wayland-client" SOVERSION 0
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/surface-client-fixture")
        add_custom_command(TARGET fgvk-surface-client-fixture POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE_NAME:fgvk-surface-client-fixture>"
                "$<TARGET_FILE_DIR:fgvk-surface-client-fixture>/libxcb.so.1"
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "$<TARGET_FILE_NAME:fgvk-surface-client-fixture>"
                "$<TARGET_FILE_DIR:fgvk-surface-client-fixture>/libX11-xcb.so.1")
        add_executable(fgvk-gamescope-scaling-surface-tests
            "tests/gamescope_scaling_surface_tests.cpp"
            "src/present_diagnostics.cpp"
            "src/gamescope_scaling_surface.cpp")
        target_include_directories(fgvk-gamescope-scaling-surface-tests
            PRIVATE "src" "../fgvk-common/include")
        target_link_libraries(fgvk-gamescope-scaling-surface-tests
            PRIVATE fgvk-surface-client-fixture fgvk-adaptive Threads::Threads ${CMAKE_DL_LIBS})
        add_test(NAME gamescope-scaling-surface
            COMMAND ${CMAKE_COMMAND} -E env
                "FGVK_PRESENT_DIAGNOSTICS=1"
                "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-surface-client-fixture>"
                $<TARGET_FILE:fgvk-gamescope-scaling-surface-tests>)
        add_test(NAME gamescope-scaling-surface-diagnostics-off
            COMMAND ${CMAKE_COMMAND} -E env
                "FGVK_PRESENT_DIAGNOSTICS=0"
                "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-surface-client-fixture>"
                $<TARGET_FILE:fgvk-gamescope-scaling-surface-tests>)
        fgvk_register_portable_policy_test(
            fgvk-gamescope-scaling-surface-tests gamescope-scaling-surface-diagnostics-off)
        fgvk_register_portable_policy_test(
            fgvk-gamescope-scaling-surface-tests gamescope-scaling-surface)
        set_tests_properties(gamescope-scaling-surface gamescope-scaling-surface-diagnostics-off
            PROPERTIES TIMEOUT 30)
    endif()

    add_executable(fgvk-adaptive-tests
        "tests/adaptive_scheduler_tests.cpp")
    target_link_libraries(fgvk-adaptive-tests
        PRIVATE fgvk-adaptive)
    add_test(NAME adaptive-scheduler
        COMMAND fgvk-adaptive-tests)
    fgvk_register_portable_policy_test(
        fgvk-adaptive-tests adaptive-scheduler)

    add_executable(fgvk-adaptive-matrix
        "tests/adaptive_scheduler_matrix.cpp")
    target_link_libraries(fgvk-adaptive-matrix
        PRIVATE fgvk-adaptive)
    add_test(NAME adaptive-scheduler-matrix
        COMMAND fgvk-adaptive-matrix)
    fgvk_register_portable_policy_test(
        fgvk-adaptive-matrix adaptive-scheduler-matrix)

    add_executable(fgvk-pnext-chain-tests
        "tests/pnext_chain_tests.cpp")
    target_include_directories(fgvk-pnext-chain-tests
        PRIVATE "src")
    add_test(NAME pnext-chain
        COMMAND fgvk-pnext-chain-tests)
    fgvk_register_portable_policy_test(fgvk-pnext-chain-tests pnext-chain)

    if(FGVK_BUILD_VK_LAYER)
        add_executable(fgvk-color-tests
            "tests/color_pipeline_tests.cpp")
        target_link_libraries(fgvk-color-tests
            PRIVATE fgvk-color)
        add_test(NAME color-pipeline
            COMMAND fgvk-color-tests)
    endif()

    add_executable(fgvk-adaptive-benchmark
        "tests/adaptive_scheduler_benchmark.cpp")
    target_link_libraries(fgvk-adaptive-benchmark
        PRIVATE fgvk-adaptive)

    add_executable(fgvk-hdr-color-math-tests
        "tests/hdr_color_math_tests.cpp")
    add_test(NAME hdr-color-math
        COMMAND fgvk-hdr-color-math-tests)
    fgvk_register_portable_policy_test(
        fgvk-hdr-color-math-tests hdr-color-math)
endif()

if(FGVK_BUILD_VK_LAYER)
    math(EXPR FGVK_LAYER_LIBRARY_ARCH "${CMAKE_SIZEOF_VOID_P} * 8")
    if(NOT FGVK_LAYER_LIBRARY_ARCH EQUAL 32 AND
            NOT FGVK_LAYER_LIBRARY_ARCH EQUAL 64)
        message(FATAL_ERROR
            "The Vulkan layer manifest supports only 32-bit or 64-bit libraries")
    endif()

    set(FGVK_LAYER_MANIFEST_FILE
        "VkLayer_FGVK_render${FGVK_LAYER_MANIFEST_SUFFIX}.json")
    set(FGVK_SCALING_LAYER_MANIFEST_FILE
        "VkLayer_FGVK_spatial_scaling${FGVK_LAYER_MANIFEST_SUFFIX}.json")
    configure_file(
        "VkLayer_FGVK_render.json.in"
        "${FGVK_LAYER_MANIFEST_FILE}"
        @ONLY)
    configure_file(
        "VkLayer_FGVK_spatial_scaling.json.in"
        "${FGVK_SCALING_LAYER_MANIFEST_FILE}"
        @ONLY)

    # framegen-vulkan-managed SDR launches use a private manifest directory, matching the
    # proven v2 launch boundary. Relative paths containing a directory need one
    # more parent traversal; a bare loader name remains valid as-is.
    set(FGVK_PUBLIC_LAYER_LIBRARY_PATH "${FGVK_LAYER_LIBRARY_PATH}")
    set(FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH
        "${FGVK_SCALING_LAYER_LIBRARY_PATH}")
    if(IS_ABSOLUTE "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
        set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    elseif(FGVK_PUBLIC_LAYER_LIBRARY_PATH MATCHES "/")
        set(FGVK_LAYER_LIBRARY_PATH "../${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    else()
        set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    endif()
    if(IS_ABSOLUTE "${FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH}")
        set(FGVK_SCALING_LAYER_LIBRARY_PATH
            "${FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH}")
    elseif(FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH MATCHES "/")
        set(FGVK_SCALING_LAYER_LIBRARY_PATH
            "../${FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH}")
    else()
        set(FGVK_SCALING_LAYER_LIBRARY_PATH
            "${FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH}")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/private-manifest")
    configure_file(
        "VkLayer_FGVK_render.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/private-manifest/${FGVK_LAYER_MANIFEST_FILE}"
        @ONLY)
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/private-scaling-manifest")
    configure_file(
        "VkLayer_FGVK_spatial_scaling.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/private-scaling-manifest/${FGVK_SCALING_LAYER_MANIFEST_FILE}"
        @ONLY)
    set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    set(FGVK_SCALING_LAYER_LIBRARY_PATH
        "${FGVK_PUBLIC_SCALING_LAYER_LIBRARY_PATH}")

    install(TARGETS fgvk-render fgvk-render-scaling
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${FGVK_LAYER_MANIFEST_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/private-manifest/${FGVK_LAYER_MANIFEST_FILE}"
        DESTINATION
            "${CMAKE_INSTALL_DATAROOTDIR}/framegen-vulkan/vulkan/implicit_layer.d")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/${FGVK_SCALING_LAYER_MANIFEST_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/private-scaling-manifest/${FGVK_SCALING_LAYER_MANIFEST_FILE}"
        DESTINATION
            "${CMAKE_INSTALL_DATAROOTDIR}/framegen-vulkan/vulkan/spatial_scaling.d")
endif()
