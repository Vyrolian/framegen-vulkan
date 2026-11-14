set(COMMON_SOURCES
    "src/configuration/config.cpp"
    "src/configuration/detection.cpp"
    "src/configuration/launch.cpp"
    "src/helpers/errors.cpp"
    "src/helpers/paths.cpp"
    "src/vulkan/buffer.cpp"
    "src/vulkan/command_buffer.cpp"
    "src/vulkan/descriptor_pool.cpp"
    "src/vulkan/descriptor_set.cpp"
    "src/vulkan/device_memory_accounting.cpp"
    "src/vulkan/fence.cpp"
    "src/vulkan/image.cpp"
    "src/vulkan/image_memory_pool.cpp"
    "src/vulkan/sampler.cpp"
    "src/vulkan/semaphore.cpp"
    "src/vulkan/shader.cpp"
    "src/vulkan/timeline_semaphore.cpp"
    "src/vulkan/vulkan.cpp")

add_library(fgvk-common STATIC ${COMMON_SOURCES})

find_package(Threads REQUIRED)
target_link_libraries(fgvk-common PUBLIC Threads::Threads)

target_include_directories(fgvk-common
    PUBLIC include)

target_include_directories(fgvk-common SYSTEM
    PRIVATE thirdparty/include)

if(BUILD_TESTING)
    add_executable(fgvk-shader-pipeline-tests "tests/shader_pipeline_tests.cpp")
    target_link_libraries(fgvk-shader-pipeline-tests PRIVATE fgvk-common)
    add_test(NAME shader-pipeline-ownership COMMAND fgvk-shader-pipeline-tests)
    set_tests_properties(shader-pipeline-ownership PROPERTIES LABELS "portable-policy")

    if(UNIX)
        add_test(NAME launcher-exclusions-freshness
            COMMAND "${FGVK_PYTHON3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/../../scripts/generate-launcher-exclusions.py"
                --check)
        set_tests_properties(launcher-exclusions-freshness PROPERTIES
            LABELS "portable-policy")
    endif()

    add_executable(fgvk-external-fd-tests "tests/external_fd_tests.cpp")
    target_link_libraries(fgvk-external-fd-tests PRIVATE fgvk-common)
    add_test(NAME external-fd-ownership COMMAND fgvk-external-fd-tests)

    add_executable(fgvk-descriptor-pool-tests "tests/descriptor_pool_tests.cpp")
    target_link_libraries(fgvk-descriptor-pool-tests PRIVATE fgvk-common)
    add_test(NAME descriptor-pool-capacity COMMAND fgvk-descriptor-pool-tests)
    set_tests_properties(external-fd-ownership descriptor-pool-capacity PROPERTIES
        LABELS "portable-policy")

    add_executable(fgvk-dll-paths-tests "tests/paths_tests.cpp")
    target_link_libraries(fgvk-dll-paths-tests PRIVATE fgvk-common)
    add_test(NAME lossless-dll-discovery COMMAND fgvk-dll-paths-tests)

    add_executable(fgvk-config-tests
        "tests/config_tests.cpp")
    target_link_libraries(fgvk-config-tests
        PRIVATE fgvk-common)
    target_include_directories(fgvk-config-tests PRIVATE src)
    add_test(NAME watched-configuration
        COMMAND fgvk-config-tests)

    add_executable(fgvk-launch-config-tests
        "tests/launch_config_tests.cpp")
    target_link_libraries(fgvk-launch-config-tests
        PRIVATE fgvk-common)
    add_test(NAME standalone-launch-configuration
        COMMAND fgvk-launch-config-tests)

    add_executable(fgvk-device-feature-tests
        "tests/device_feature_tests.cpp")
    target_link_libraries(fgvk-device-feature-tests
        PRIVATE fgvk-common)
    add_test(NAME optional-device-features
        COMMAND fgvk-device-feature-tests)

    add_executable(fgvk-device-memory-accounting-tests
        "tests/device_memory_accounting_tests.cpp")
    target_link_libraries(fgvk-device-memory-accounting-tests
        PRIVATE fgvk-common)
    add_test(NAME device-memory-accounting
        COMMAND fgvk-device-memory-accounting-tests)

    add_executable(fgvk-image-memory-pool-tests
        "tests/image_memory_pool_tests.cpp")
    target_link_libraries(fgvk-image-memory-pool-tests
        PRIVATE fgvk-common)
    add_test(NAME image-memory-pool
        COMMAND fgvk-image-memory-pool-tests)

    add_executable(fgvk-command-buffer-submit-storage-tests
        "tests/command_buffer_submit_storage_tests.cpp")
    target_include_directories(fgvk-command-buffer-submit-storage-tests
        PRIVATE src)
    target_link_libraries(fgvk-command-buffer-submit-storage-tests PRIVATE fgvk-common)
    add_test(NAME command-buffer-submit-storage
        COMMAND fgvk-command-buffer-submit-storage-tests)
    set_tests_properties(command-buffer-submit-storage PROPERTIES LABELS "portable-policy")

    add_executable(fgvk-gpu-clock-tests
        "tests/gpu_clock_tests.cpp")
    target_link_libraries(fgvk-gpu-clock-tests PRIVATE fgvk-common)
    add_test(NAME gpu-clock-mapping COMMAND fgvk-gpu-clock-tests)
endif()

if(FGVK_INSTALL_DEVELOP)
    install(TARGETS fgvk-common
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "include/fgvk-common/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fgvk-common"
        FILES_MATCHING PATTERN "*.hpp")
endif()
