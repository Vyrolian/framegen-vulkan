add_library(fgvk-cli-support STATIC
    "src/arguments.cpp"
    "src/i18n.cpp")
target_include_directories(fgvk-cli-support
    PUBLIC "src")

set(CLI_SOURCES
    "src/tools/inspect_dll.cpp"
    "src/tools/mark.cpp"
    "src/tools/validate.cpp"
    "src/main.cpp")

add_executable(fgvk-cli ${CLI_SOURCES})

target_link_libraries(fgvk-cli
    PRIVATE fgvk-cli-support
    PUBLIC fgvk-common
    PUBLIC fgvk-backend)

target_compile_options(fgvk-cli PRIVATE
    -Wno-unknown-warning-option
    -Wno-unsafe-buffer-usage) # CLI parsing

if(BUILD_TESTING)
    add_executable(fgvk-cli-dll-inspection-tests
        "src/tools/inspect_dll.cpp"
        "tests/inspect_dll_tests.cpp")
    target_include_directories(fgvk-cli-dll-inspection-tests PRIVATE
        "src" "../fgvk-backend/include")
    add_test(NAME cli-dll-inspection COMMAND fgvk-cli-dll-inspection-tests)

    add_executable(fgvk-cli-i18n-tests
        "tests/i18n_tests.cpp")
    target_link_libraries(fgvk-cli-i18n-tests
        PRIVATE fgvk-cli-support)
    add_test(NAME cli-i18n-unit
        COMMAND fgvk-cli-i18n-tests)

    add_test(NAME cli-i18n-contract
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-i18n-cli.sh"
            "$<TARGET_FILE:fgvk-cli>")

    # Exercise the production parser and option defaults without a GPU or DLL.
    add_executable(fgvk-cli-precision-probe
        "src/main.cpp"
        "src/tools/mark.cpp"
        "tests/precision_cli_stubs.cpp")
    target_link_libraries(fgvk-cli-precision-probe PRIVATE fgvk-cli-support)
    target_include_directories(fgvk-cli-precision-probe PRIVATE
        "../fgvk-backend/include")
    add_test(NAME cli-precision-contract
        COMMAND bash
            "${CMAKE_CURRENT_SOURCE_DIR}/tests/test-precision-cli.sh"
            "$<TARGET_FILE:fgvk-cli-precision-probe>")

endif()

install(TARGETS fgvk-cli
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
