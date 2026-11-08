# Native Remote Play is x86_64-only. The helper travels with its shared Python
# owner in standalone installs, independent of lib/lib32 layout.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SIZEOF_VOID_P EQUAL 8 AND
        CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    add_library(fgvk-remote-play-sdr SHARED sdr_helper.cpp)
    target_link_libraries(fgvk-remote-play-sdr PRIVATE ${CMAKE_DL_LIBS})
    set_target_properties(fgvk-remote-play-sdr PROPERTIES
        CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
    install(TARGETS fgvk-remote-play-sdr LIBRARY DESTINATION
        "${CMAKE_INSTALL_DATAROOTDIR}/framegen-vulkan/fgvk_remote_play")

    if(BUILD_TESTING)
        add_library(fgvk-remote-play-sdl-fixture SHARED tests/sdl_fixture.cpp)
        add_executable(fgvk-remote-play-sdr-tests tests/sdr_tests.cpp)
        target_link_libraries(fgvk-remote-play-sdr-tests PRIVATE fgvk-remote-play-sdl-fixture)
        # This executable is a synthetic client; it never creates a real window.
        set_target_properties(fgvk-remote-play-sdr-tests PROPERTIES
            OUTPUT_NAME streaming_client.fgvk-original)
        add_custom_command(TARGET fgvk-remote-play-sdr-tests POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy $<TARGET_FILE:fgvk-remote-play-sdr-tests>
                "$<TARGET_FILE_DIR:fgvk-remote-play-sdr-tests>/ordinary-client")
        set(sdr_test_environment "LD_PRELOAD=$<TARGET_FILE:fgvk-remote-play-sdr>"
            "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:fgvk-remote-play-sdl-fixture>")
        if(FGVK_ENABLE_SANITIZERS)
            # The test deliberately preloads an instrumented library ahead of
            # executable DT_NEEDED entries; both use the same sanitizer runtime.
            list(APPEND sdr_test_environment "ASAN_OPTIONS=verify_asan_link_order=0")
        endif()
        add_test(NAME remote-play-sdr COMMAND "${CMAKE_COMMAND}" -E env
            ${sdr_test_environment}
            "FGVK_REMOTE_PLAY_SDR=1" $<TARGET_FILE:fgvk-remote-play-sdr-tests> forced)
        add_test(NAME remote-play-sdr-inactive COMMAND "${CMAKE_COMMAND}" -E env
            ${sdr_test_environment}
            "FGVK_REMOTE_PLAY_SDR=0" $<TARGET_FILE:fgvk-remote-play-sdr-tests> native)
        set_tests_properties(remote-play-sdr remote-play-sdr-inactive PROPERTIES LABELS "portable-policy")
        add_test(NAME remote-play-sdr-child-isolation COMMAND "${CMAKE_COMMAND}" -E env
            ${sdr_test_environment} "FGVK_REMOTE_PLAY_SDR=1"
            "$<TARGET_FILE_DIR:fgvk-remote-play-sdr-tests>/ordinary-client" native)
        set_tests_properties(remote-play-sdr-child-isolation PROPERTIES LABELS "portable-policy")
        foreach(phase lock copy enumerate pointer set create actual)
            add_test(NAME remote-play-sdr-failure-${phase} COMMAND "${CMAKE_COMMAND}" -E env
                ${sdr_test_environment} "FGVK_REMOTE_PLAY_SDR=1" "FGVK_SDL_FIXTURE_PHASE=${phase}"
                $<TARGET_FILE:fgvk-remote-play-sdr-tests> failure)
            set_tests_properties(remote-play-sdr-failure-${phase} PROPERTIES LABELS "portable-policy")
        endforeach()
        add_dependencies(fgvk-portable-policy-test-binaries
            fgvk-remote-play-sdr fgvk-remote-play-sdr-tests)
    endif()
endif()
