# The pacing recorder: the bounded record queue, the writer that formats it, and the pin that keeps
# the layer mapped while that writer thread runs.
set(PRESENT_RECORD_SOURCES
    "src/trace_recorder.cpp"
    "src/present_records.cpp"
    "src/library_pin.cpp")

set(LAYER_SOURCES
    "src/entrypoint.cpp"
    "src/instance.cpp"
    "src/present_diagnostics.cpp"
    ${PRESENT_RECORD_SOURCES}
    "src/wayland_presentation_feedback.cpp"
    "src/wayland_output_refresh.cpp"
    "src/x11_refresh.cpp"
    "src/swapchain/create.cpp"
    "src/swapchain/onscreen_watch.cpp"
    "src/swapchain/internal_images.cpp"
    "src/swapchain/present_layout.cpp"
    "src/swapchain/present_marker.cpp"
    "src/swapchain/present.cpp"
    "src/swapchain/present/admission.cpp"
    "src/swapchain/present/generated_frames.cpp"
    "src/swapchain/present/real_frames.cpp"
    "src/swapchain/present/retirement.cpp"
    "src/swapchain/profile.cpp"
    "src/swapchain/resources.cpp")

set(COLOR_PIPELINE_SOURCE "src/color_pipeline.cpp")

# Keep explicit target membership while detecting a newly added implementation
# that was forgotten in CMake. The checker runs for the default and layer
# targets, so adding a source under src/ fails even in an existing build tree.
set(RENDERER_PRODUCTION_SOURCES
    ${LAYER_SOURCES}
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
            PRIVATE fgvk-color
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

endif()

if(BUILD_TESTING)
    add_test(NAME renderer-source-inventory
        COMMAND "${CMAKE_COMMAND}" -P
            "${CMAKE_CURRENT_BINARY_DIR}/check-source-inventory.cmake")
    set_tests_properties(renderer-source-inventory PROPERTIES
        LABELS "portable-policy")

    # Keep portable build and CTest membership together so the shell runner
    # cannot drift when another policy test is added.
    add_custom_target(fgvk-portable-policy-test-binaries)
    function(fgvk_register_portable_policy_test target test_name)
        add_dependencies(fgvk-portable-policy-test-binaries ${target})
        set_tests_properties(${test_name} PROPERTIES
            LABELS "portable-policy")
    endfunction()

    add_dependencies(fgvk-portable-policy-test-binaries
        fgvk-external-fd-tests fgvk-descriptor-pool-tests fgvk-shader-pipeline-tests
        fgvk-command-buffer-submit-storage-tests fgvk-backend-queue-priority-tests)

    add_library(fgvk-launch-preload-fixture SHARED
        "../scripts/test-launch-preload.cpp")
    target_compile_definitions(fgvk-launch-preload-fixture PRIVATE FGVK_TEST_RETAINED)
    add_executable(fgvk-steam-overlay-diagnostics-tests
        "tests/steam_overlay_libraries_tests.cpp" "src/present_diagnostics.cpp" ${PRESENT_RECORD_SOURCES})
    target_include_directories(fgvk-steam-overlay-diagnostics-tests PRIVATE "src")
    target_link_libraries(fgvk-steam-overlay-diagnostics-tests
        PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(fgvk-steam-overlay-diagnostics-tests PRIVATE "../fgvk-common/include")
    add_dependencies(fgvk-steam-overlay-diagnostics-tests fgvk-launch-preload-fixture)
    add_test(NAME steam-overlay-library-diagnostics
        COMMAND fgvk-steam-overlay-diagnostics-tests $<TARGET_FILE:fgvk-launch-preload-fixture>)
    fgvk_register_portable_policy_test(fgvk-steam-overlay-diagnostics-tests steam-overlay-library-diagnostics)

    add_executable(fgvk-pacing-profile-tests
        "tests/pacing_profile_tests.cpp" "src/present_diagnostics.cpp" ${PRESENT_RECORD_SOURCES})
    target_include_directories(fgvk-pacing-profile-tests PRIVATE "src")
    target_link_libraries(fgvk-pacing-profile-tests PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(fgvk-pacing-profile-tests PRIVATE "../fgvk-common/include")
    add_test(NAME pacing-profile COMMAND fgvk-pacing-profile-tests)
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests pacing-profile)

    # The layer pin: the module starts a detached thread inside its own DSO, then the executable
    # unloads it. Without the pin the thread wakes into unmapped code and the test crashes.
    add_library(fgvk-library-pin-module MODULE
        "src/library_pin.cpp" "tests/library_pin_tests.cpp")
    target_include_directories(fgvk-library-pin-module PRIVATE "src")
    target_compile_definitions(fgvk-library-pin-module PRIVATE FGVK_LIBRARY_PIN_MODULE)
    target_link_libraries(fgvk-library-pin-module PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    add_executable(fgvk-library-pin-tests "tests/library_pin_tests.cpp")
    target_include_directories(fgvk-library-pin-tests PRIVATE "src")
    target_link_libraries(fgvk-library-pin-tests PRIVATE ${CMAKE_DL_LIBS})
    add_dependencies(fgvk-library-pin-tests fgvk-library-pin-module)
    add_test(NAME library-pin COMMAND fgvk-library-pin-tests $<TARGET_FILE:fgvk-library-pin-module>)
    fgvk_register_portable_policy_test(fgvk-library-pin-tests library-pin)

    add_test(NAME pacing-profile-report
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_pacing_profile_report.py")
    fgvk_register_portable_policy_test(fgvk-pacing-profile-tests pacing-profile-report)
    # The pacing writer runs a thread, so every target that compiles it links the thread library.
    foreach(fgvk_recorder_test IN ITEMS fgvk-steam-overlay-diagnostics-tests fgvk-pacing-profile-tests)
        target_link_libraries(${fgvk_recorder_test} PRIVATE Threads::Threads)
    endforeach()
    add_executable(fgvk-pacing-record-golden-tests
        "tests/pacing_record_golden_tests.cpp" "src/present_diagnostics.cpp" ${PRESENT_RECORD_SOURCES})
    target_include_directories(fgvk-pacing-record-golden-tests PRIVATE "src")
    target_link_libraries(fgvk-pacing-record-golden-tests PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(fgvk-pacing-record-golden-tests PRIVATE "../fgvk-common/include")
    add_test(NAME pacing-record-golden COMMAND fgvk-pacing-record-golden-tests
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/pacing-record-golden.txt")
    fgvk_register_portable_policy_test(fgvk-pacing-record-golden-tests pacing-record-golden)
    add_executable(fgvk-trace-recorder-tests "tests/trace_recorder_tests.cpp" "src/trace_recorder.cpp")
    target_include_directories(fgvk-trace-recorder-tests PRIVATE "src")
    target_link_libraries(fgvk-trace-recorder-tests PRIVATE Threads::Threads)
    add_test(NAME trace-recorder COMMAND fgvk-trace-recorder-tests)
    fgvk_register_portable_policy_test(fgvk-trace-recorder-tests trace-recorder)
    add_executable(fgvk-pacing-profile-benchmark
        "tests/pacing_profile_benchmark.cpp" "src/present_diagnostics.cpp" ${PRESENT_RECORD_SOURCES})
    target_include_directories(fgvk-pacing-profile-benchmark PRIVATE "src")
    target_link_libraries(fgvk-pacing-profile-benchmark PRIVATE ${CMAKE_DL_LIBS})
    target_include_directories(fgvk-pacing-profile-benchmark PRIVATE "../fgvk-common/include")
    target_link_libraries(fgvk-pacing-profile-benchmark PRIVATE Threads::Threads)

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

    add_executable(fgvk-layer-role-tests
        "tests/layer_role_tests.cpp")
    target_include_directories(fgvk-layer-role-tests
        PRIVATE "src"
        PRIVATE "../fgvk-common/include")
    add_test(NAME layer-role
        COMMAND fgvk-layer-role-tests)
    fgvk_register_portable_policy_test(
        fgvk-layer-role-tests layer-role)

    add_executable(fgvk-presentation-policy-tests
        "tests/presentation_policy_tests.cpp")
    target_include_directories(fgvk-presentation-policy-tests
        PRIVATE "src")
    add_test(NAME presentation-policy
        COMMAND fgvk-presentation-policy-tests)
    fgvk_register_portable_policy_test(
        fgvk-presentation-policy-tests presentation-policy)

    add_executable(fgvk-deferred-present-tests
        "tests/deferred_present_tests.cpp" "src/present_diagnostics.cpp" "src/swapchain/onscreen_watch.cpp"
        ${PRESENT_RECORD_SOURCES})
    target_include_directories(fgvk-deferred-present-tests PRIVATE "src")
    target_link_libraries(fgvk-deferred-present-tests PRIVATE ${CMAKE_DL_LIBS} Threads::Threads)
    target_include_directories(fgvk-deferred-present-tests PRIVATE "../fgvk-common/include")
    add_test(NAME deferred-present COMMAND fgvk-deferred-present-tests)
    fgvk_register_portable_policy_test(fgvk-deferred-present-tests deferred-present)

    # The hitch analyser is standard-library Python; its synthetic capture is checked here.
    add_test(NAME analyze-hitches
        COMMAND "${FGVK_PYTHON3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_hitches.py")
    set_tests_properties(analyze-hitches PROPERTIES LABELS "portable-policy")

    add_executable(fgvk-release-gate-tests "tests/release_gate_tests.cpp")
    target_include_directories(fgvk-release-gate-tests PRIVATE "src")
    target_link_libraries(fgvk-release-gate-tests PRIVATE Threads::Threads)
    add_test(NAME release-gate COMMAND fgvk-release-gate-tests)
    fgvk_register_portable_policy_test(fgvk-release-gate-tests release-gate)

    # Just-in-time release: the pure delay controller and the native acquire hold.
    add_executable(fgvk-low-latency-tests "tests/low_latency_tests.cpp")
    target_include_directories(fgvk-low-latency-tests PRIVATE "src")
    add_test(NAME low-latency COMMAND fgvk-low-latency-tests)
    fgvk_register_portable_policy_test(fgvk-low-latency-tests low-latency)

    # Just-in-time batch start: the phase lock that places each released batch after its readiness.
    add_executable(fgvk-batch-phase-tests "tests/batch_phase_tests.cpp")
    target_include_directories(fgvk-batch-phase-tests PRIVATE "src")
    add_test(NAME batch-phase COMMAND fgvk-batch-phase-tests)
    fgvk_register_portable_policy_test(fgvk-batch-phase-tests batch-phase)

    # GP1: the on-screen gate, the lower present ID owner and the watcher (fake wait, no driver).
    add_executable(fgvk-onscreen-profile-tests "tests/onscreen_profile_tests.cpp" "src/swapchain/onscreen_watch.cpp")
    target_include_directories(fgvk-onscreen-profile-tests PRIVATE "src")
    target_link_libraries(fgvk-onscreen-profile-tests PRIVATE Threads::Threads)
    add_test(NAME onscreen-profile COMMAND fgvk-onscreen-profile-tests)
    fgvk_register_portable_policy_test(fgvk-onscreen-profile-tests onscreen-profile)

    # GT1: the gamescope tick grid, the lead and the tick choice (pure; static_asserts and a runtime main).
    add_executable(fgvk-tick-lock-tests "tests/tick_lock_tests.cpp")
    target_include_directories(fgvk-tick-lock-tests PRIVATE "src")
    add_test(NAME tick-lock COMMAND fgvk-tick-lock-tests)
    fgvk_register_portable_policy_test(fgvk-tick-lock-tests tick-lock)

    # OS1: the screen sample ring, the producer mode and the source rule (pure).
    add_executable(fgvk-screen-samples-tests "tests/screen_samples_tests.cpp")
    target_include_directories(fgvk-screen-samples-tests PRIVATE "src")
    target_link_libraries(fgvk-screen-samples-tests PRIVATE Threads::Threads)
    add_test(NAME screen-samples COMMAND fgvk-screen-samples-tests)
    fgvk_register_portable_policy_test(fgvk-screen-samples-tests screen-samples)

    # Reflex frame limit: the pure interval rule and the per-swapchain request state.
    add_executable(fgvk-reflex-frame-limit-tests "tests/reflex_frame_limit_tests.cpp")
    target_include_directories(fgvk-reflex-frame-limit-tests PRIVATE "src")
    target_link_libraries(fgvk-reflex-frame-limit-tests PRIVATE Threads::Threads)
    add_test(NAME reflex-frame-limit COMMAND fgvk-reflex-frame-limit-tests)
    fgvk_register_portable_policy_test(fgvk-reflex-frame-limit-tests reflex-frame-limit)

    # Reflex present attribution: the application's marker rule, the present's ID and the lower out-of-band pair.
    add_executable(fgvk-reflex-present-attribution-tests "tests/reflex_present_attribution_tests.cpp")
    target_include_directories(fgvk-reflex-present-attribution-tests PRIVATE "src")
    add_test(NAME reflex-present-attribution COMMAND fgvk-reflex-present-attribution-tests)
    fgvk_register_portable_policy_test(fgvk-reflex-present-attribution-tests reflex-present-attribution)

    # RX1: the game's submission frame tag transitions and the chain walk (pure).
    add_executable(fgvk-submit-tag-tracker-tests "tests/submit_tag_tracker_tests.cpp")
    target_include_directories(fgvk-submit-tag-tracker-tests PRIVATE "src")
    add_test(NAME submit-tag-tracker COMMAND fgvk-submit-tag-tracker-tests)
    fgvk_register_portable_policy_test(fgvk-submit-tag-tracker-tests submit-tag-tracker)

    # RP1: the real present lead's marker pairing, error rule and limits (pure).
    add_executable(fgvk-real-present-lead-tests "tests/real_present_lead_tests.cpp")
    target_include_directories(fgvk-real-present-lead-tests PRIVATE "src")
    add_test(NAME real-present-lead COMMAND fgvk-real-present-lead-tests)
    fgvk_register_portable_policy_test(fgvk-real-present-lead-tests real-present-lead)

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

    add_executable(fgvk-present-layout-tests
        "tests/present_layout_tests.cpp" "src/swapchain/present_layout.cpp")
    target_include_directories(fgvk-present-layout-tests PRIVATE "src")
    target_link_libraries(fgvk-present-layout-tests PRIVATE Threads::Threads)
    add_test(NAME present-layout COMMAND fgvk-present-layout-tests)
    fgvk_register_portable_policy_test(fgvk-present-layout-tests present-layout)

    # QH2: the transfer family choice, the transfer queue request and the queue families a lower swapchain
    # shares with it. Pure policy, so no device is needed.
    add_executable(fgvk-transfer-queue-tests "tests/transfer_queue_tests.cpp")
    target_include_directories(fgvk-transfer-queue-tests PRIVATE "src")
    add_test(NAME transfer-queue COMMAND fgvk-transfer-queue-tests)
    fgvk_register_portable_policy_test(fgvk-transfer-queue-tests transfer-queue)

    # fgvk-backend is configured only for layer, CLI or develop builds. Its
    # fence ring is header-only, so the portable policy set builds this backend
    # test from the backend tree and keeps it in the same label.
    add_executable(fgvk-backend-fence-ring-tests "../fgvk-backend/tests/fence_ring_tests.cpp")
    target_include_directories(fgvk-backend-fence-ring-tests PRIVATE "../fgvk-backend/src")
    add_test(NAME backend-fence-ring COMMAND fgvk-backend-fence-ring-tests)
    fgvk_register_portable_policy_test(fgvk-backend-fence-ring-tests backend-fence-ring)

    # GP2 grouping: the in-order run partition behind ManagedShader::dispatchGroups, and the
    # FGVK_BACKEND_OVERLAP parser. Synthetic barrier lists only; no Vulkan device.
    add_executable(fgvk-backend-dispatch-groups-tests "../fgvk-backend/tests/dispatch_groups_tests.cpp")
    target_include_directories(fgvk-backend-dispatch-groups-tests PRIVATE "../fgvk-backend/src" "../fgvk-common/include")
    add_test(NAME backend-dispatch-groups COMMAND fgvk-backend-dispatch-groups-tests)
    fgvk_register_portable_policy_test(fgvk-backend-dispatch-groups-tests backend-dispatch-groups)

    add_executable(fgvk-uncapped-auto-cap-tests "tests/uncapped_auto_cap_tests.cpp")
    target_include_directories(fgvk-uncapped-auto-cap-tests PRIVATE "src")
    add_test(NAME uncapped-auto-cap COMMAND fgvk-uncapped-auto-cap-tests)
    fgvk_register_portable_policy_test(fgvk-uncapped-auto-cap-tests uncapped-auto-cap)

    # X5 refresh: the RandR mode rate, CRTC choice and Wayland output name match. The X and Wayland
    # I/O has no seam and is not tested here.
    add_executable(fgvk-x11-refresh-policy-tests "tests/x11_refresh_policy_tests.cpp")
    target_include_directories(fgvk-x11-refresh-policy-tests PRIVATE "src")
    add_test(NAME x11-refresh-policy COMMAND fgvk-x11-refresh-policy-tests)
    fgvk_register_portable_policy_test(fgvk-x11-refresh-policy-tests x11-refresh-policy)

    # Completion spiral: the log-only rule over one-second windows of source presents. It
    # never changes pacing, so it has no pacing fixture.
    add_executable(fgvk-completion-spiral-tests "tests/completion_spiral_tests.cpp")
    target_include_directories(fgvk-completion-spiral-tests PRIVATE "src")
    add_test(NAME completion-spiral COMMAND fgvk-completion-spiral-tests)
    fgvk_register_portable_policy_test(fgvk-completion-spiral-tests completion-spiral)

    # Surface format log (H2a): the dedupe key, the line format, the complete-answer rule and the
    # per-process cap. The pass-through wrappers need a Vulkan device and are not tested here.
    add_executable(fgvk-surface-formats-tests "tests/surface_formats_tests.cpp")
    target_include_directories(fgvk-surface-formats-tests PRIVATE "src")
    add_test(NAME surface-formats COMMAND fgvk-surface-formats-tests)
    fgvk_register_portable_policy_test(fgvk-surface-formats-tests surface-formats)

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
            "tests/wayland_presentation_feedback_tests.cpp" "src/wayland_presentation_feedback.cpp" "src/present_diagnostics.cpp" ${PRESENT_RECORD_SOURCES})
        target_include_directories(fgvk-wayland-presentation-tests PRIVATE src)
        target_link_libraries(fgvk-wayland-presentation-tests PRIVATE fgvk-wayland-presentation-fixture Threads::Threads ${CMAKE_DL_LIBS})
        target_include_directories(fgvk-wayland-presentation-tests PRIVATE "../fgvk-common/include")
        add_test(NAME wayland-presentation-feedback COMMAND ${CMAKE_COMMAND} -E env
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-wayland-presentation-fixture>" $<TARGET_FILE:fgvk-wayland-presentation-tests>)
        fgvk_register_portable_policy_test(fgvk-wayland-presentation-tests wayland-presentation-feedback)

    endif()

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
    configure_file(
        "VkLayer_FGVK_render.json.in"
        "${FGVK_LAYER_MANIFEST_FILE}"
        @ONLY)

    # framegen-vulkan-managed SDR launches use a private manifest directory, matching the
    # proven v2 launch boundary. Relative paths containing a directory need one
    # more parent traversal; a bare loader name remains valid as-is.
    set(FGVK_PUBLIC_LAYER_LIBRARY_PATH "${FGVK_LAYER_LIBRARY_PATH}")
    if(IS_ABSOLUTE "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
        set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    elseif(FGVK_PUBLIC_LAYER_LIBRARY_PATH MATCHES "/")
        set(FGVK_LAYER_LIBRARY_PATH "../${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    else()
        set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")
    endif()
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/private-manifest")
    configure_file(
        "VkLayer_FGVK_render.json.in"
        "${CMAKE_CURRENT_BINARY_DIR}/private-manifest/${FGVK_LAYER_MANIFEST_FILE}"
        @ONLY)
    set(FGVK_LAYER_LIBRARY_PATH "${FGVK_PUBLIC_LAYER_LIBRARY_PATH}")

    install(TARGETS fgvk-render
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/${FGVK_LAYER_MANIFEST_FILE}"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d")
    install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/private-manifest/${FGVK_LAYER_MANIFEST_FILE}"
        DESTINATION
            "${CMAKE_INSTALL_DATAROOTDIR}/framegen-vulkan/vulkan/implicit_layer.d")
endif()
