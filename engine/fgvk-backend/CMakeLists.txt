set(BACKEND_SOURCES
    "src/extraction/content_hash.cpp"
    "src/extraction/dll_inspection.cpp"
    "src/extraction/dll_reader.cpp"
    "src/extraction/dxbc_translation.cpp"
    "src/extraction/ls1_shader_set.cpp"
    "src/extraction/spirv_image_format.cpp"
    "src/extraction/spirv_output_split.cpp"
    "src/extraction/spirv_fp16.cpp"
    "src/extraction/lsfg_shader_set.cpp"
    "src/extraction/model_resource_validation.cpp"
    "src/extraction/model_resources.cpp"
    "src/extraction/shader_registry.cpp"
    "src/helpers/limits.cpp"
    "src/helpers/managed_shader.cpp"
    "src/helpers/utils.cpp"
    "src/shaderchains/alpha0.cpp"
    "src/shaderchains/alpha1.cpp"
    "src/shaderchains/beta0.cpp"
    "src/shaderchains/beta1.cpp"
    "src/shaderchains/delta0.cpp"
    "src/shaderchains/delta1.cpp"
    "src/shaderchains/gamma0.cpp"
    "src/shaderchains/gamma1.cpp"
    "src/shaderchains/generate.cpp"
    "src/shaderchains/mipmaps.cpp"
    "src/fgvk.cpp")

add_library(fgvk-backend STATIC ${BACKEND_SOURCES})

target_include_directories(fgvk-backend
    PUBLIC include)

target_link_libraries(fgvk-backend
    PUBLIC fgvk-common
    PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(fgvk-backend PROPERTIES
    CXX_VISIBILITY_PRESET hidden)

if(FGVK_EXPERIMENTAL_LSFG_FP16)
    if(NOT EXISTS "${FGVK_LSFG_FP16_TOOLS_SOURCE}/include/spirv-tools/optimizer.hpp")
        message(FATAL_ERROR "Prepare SPIRV-Tools with scripts/prepare-lsfg-fp16-tools.sh and set FGVK_LSFG_FP16_TOOLS_SOURCE")
    endif()
    set(SPIRV_SKIP_TESTS ON CACHE BOOL "" FORCE)
    set(SPIRV_SKIP_EXECUTABLES ON CACHE BOOL "" FORCE)
    set(SKIP_SPIRV_TOOLS_INSTALL ON CACHE BOOL "" FORCE)
    set(SPIRV_TOOLS_BUILD_STATIC ON CACHE BOOL "" FORCE)
    set(SPIRV_TOOLS_LIBRARY_TYPE STATIC CACHE STRING "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(SPIRV_WERROR OFF CACHE BOOL "" FORCE)
    add_subdirectory("${FGVK_LSFG_FP16_TOOLS_SOURCE}" "spirv-tools" EXCLUDE_FROM_ALL)
    target_link_libraries(fgvk-backend PRIVATE SPIRV-Tools-opt)
    # A Vulkan layer must not export the statically included optimizer into
    # the application's symbol namespace or interpose on another layer.
    if(UNIX AND NOT APPLE)
        target_link_libraries(fgvk-backend PRIVATE
            "-Wl,--exclude-libs,libSPIRV-Tools-opt.a:libSPIRV-Tools.a")
    endif()
    target_compile_definitions(fgvk-backend PRIVATE FGVK_EXPERIMENTAL_LSFG_FP16=1)
    install(FILES "${FGVK_LSFG_FP16_TOOLS_SOURCE}/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/framegen-vulkan/spirv-tools")
    install(FILES "${FGVK_LSFG_FP16_TOOLS_SOURCE}/external/spirv-headers/LICENSE"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/framegen-vulkan/spirv-headers")
endif()

if(BUILD_TESTING)
    add_executable(fgvk-black-image-tests "tests/black_image_tests.cpp")
    target_include_directories(fgvk-black-image-tests PRIVATE src)
    target_link_libraries(fgvk-black-image-tests PRIVATE fgvk-common)
    add_test(NAME backend-black-image COMMAND fgvk-black-image-tests)
    set_tests_properties(backend-black-image PROPERTIES LABELS "portable-policy")

    add_executable(fgvk-backend-output-split-tests
        "tests/spirv_output_split_tests.cpp"
        "src/extraction/spirv_output_split.cpp"
        "src/extraction/model_resource_validation.cpp")
    target_include_directories(fgvk-backend-output-split-tests PRIVATE src include)
    target_link_libraries(fgvk-backend-output-split-tests PRIVATE fgvk-common)
    add_test(NAME backend-output-split COMMAND fgvk-backend-output-split-tests)
    set_tests_properties(backend-output-split PROPERTIES LABELS "portable-policy")

    if(FGVK_EXPERIMENTAL_LSFG_FP16)
        add_executable(fgvk-backend-fp16-conversion-tests "tests/spirv_fp16_tests.cpp")
        target_include_directories(fgvk-backend-fp16-conversion-tests PRIVATE src)
        target_link_libraries(fgvk-backend-fp16-conversion-tests PRIVATE fgvk-backend SPIRV-Tools-opt)
        add_test(NAME backend-experimental-lsfg-fp16 COMMAND fgvk-backend-fp16-conversion-tests)
    endif()
    add_executable(fgvk-independent-dispatch-tests "tests/independent_dispatch_tests.cpp")
    target_include_directories(fgvk-independent-dispatch-tests PRIVATE src)
    target_link_libraries(fgvk-independent-dispatch-tests PRIVATE fgvk-common)
    add_test(NAME backend-independent-dispatch COMMAND fgvk-independent-dispatch-tests)
    set_tests_properties(backend-independent-dispatch PROPERTIES LABELS "portable-policy")

    add_executable(fgvk-backend-hot-path-tests
        "tests/hot_path_tests.cpp")
    target_include_directories(fgvk-backend-hot-path-tests
        PRIVATE src include)
    add_test(NAME backend-hot-path
        COMMAND fgvk-backend-hot-path-tests)

    add_executable(fgvk-backend-ls1-spirv-patch-tests
        "tests/ls1_spirv_patch_tests.cpp"
        "src/extraction/spirv_image_format.cpp")
    target_include_directories(fgvk-backend-ls1-spirv-patch-tests
        PRIVATE src)
    target_link_libraries(fgvk-backend-ls1-spirv-patch-tests
        PRIVATE fgvk-common)
    add_test(NAME backend-ls1-spirv-patch
        COMMAND fgvk-backend-ls1-spirv-patch-tests)

    add_executable(fgvk-backend-dll-inspection-tests
        "tests/dll_inspection_tests.cpp")
    add_library(fgvk-test-dxbc-translator SHARED "tests/dxbc_translator_fixture.cpp")
    target_include_directories(fgvk-test-dxbc-translator PRIVATE src)
    add_dependencies(fgvk-backend-dll-inspection-tests fgvk-test-dxbc-translator)
    target_compile_definitions(fgvk-backend-dll-inspection-tests PRIVATE
        FGVK_TEST_TRANSLATOR_PATH="$<TARGET_FILE:fgvk-test-dxbc-translator>")
    target_include_directories(fgvk-backend-dll-inspection-tests
        PRIVATE src)
    target_link_libraries(fgvk-backend-dll-inspection-tests
        PRIVATE fgvk-backend)
    add_test(NAME backend-dll-inspection
        COMMAND fgvk-backend-dll-inspection-tests)

endif()

if(FGVK_INSTALL_DEVELOP)
    install(TARGETS fgvk-backend
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
    install(DIRECTORY "include/fgvk-backend/"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/fgvk-backend"
        FILES_MATCHING PATTERN "*.hpp")
endif()
