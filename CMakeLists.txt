cmake_minimum_required(VERSION 3.24)
project(mcp_observatory VERSION 0.6.0 LANGUAGES CXX)

option(MCPO_BUILD_TESTS "Build focused tests" ON)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/rules")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/rules/artifact-static-analysis-v1.json"
    "${CMAKE_CURRENT_BINARY_DIR}/rules/artifact-static-analysis-v1.json"
    COPYONLY
)

find_package(SQLite3 REQUIRED)

execute_process(
    COMMAND git rev-parse --verify HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE MCPO_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT MCPO_GIT_COMMIT)
    set(MCPO_GIT_COMMIT "unknown")
endif()

add_library(mcpo_core STATIC
    src/acquisition.cpp
    src/analyze.cpp
    src/catalog_lock.cpp
    src/explorer.cpp
    src/history.cpp
    src/inventory.cpp
    src/observation.cpp
    src/registry.cpp
    src/target_manifest.cpp
)
target_include_directories(mcpo_core PUBLIC include)
target_compile_features(mcpo_core PUBLIC cxx_std_20)
target_compile_definitions(mcpo_core PRIVATE
    MCPO_VERSION="${PROJECT_VERSION}"
    MCPO_GIT_COMMIT="${MCPO_GIT_COMMIT}"
)
target_compile_options(mcpo_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)
target_link_libraries(mcpo_core PRIVATE SQLite::SQLite3)

add_executable(mcp-observatory src/main.cpp)
target_link_libraries(mcp-observatory PRIVATE mcpo_core)
target_compile_features(mcp-observatory PRIVATE cxx_std_20)

if(MCPO_BUILD_TESTS)
    enable_testing()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_executable(mcpo_tests tests/test_main.cpp)
    target_link_libraries(mcpo_tests PRIVATE mcpo_core)
    target_compile_features(mcpo_tests PRIVATE cxx_std_20)
    add_test(NAME mcpo_tests COMMAND mcpo_tests)

    add_executable(mcpo_acquisition_tests tests/test_acquisition.cpp)
    target_link_libraries(mcpo_acquisition_tests PRIVATE mcpo_core)
    target_compile_features(mcpo_acquisition_tests PRIVATE cxx_std_20)
    add_test(NAME mcpo_acquisition_tests COMMAND mcpo_acquisition_tests)

    add_test(
        NAME mcpo_registry_cli_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registry_cli.py
            $<TARGET_FILE:mcp-observatory>
    )
    add_test(
        NAME mcpo_explorer_cli_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_explorer_cli.py
            $<TARGET_FILE:mcp-observatory>
    )
    add_test(
        NAME mcpo_registry_refresh_cli_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_refresh_cli.py
            $<TARGET_FILE:mcp-observatory>
    )
    add_test(
        NAME mcpo_registry_maintenance_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_registry_maintenance.py
            $<TARGET_FILE:mcp-observatory>
            ${CMAKE_CURRENT_SOURCE_DIR}/tools/registry_maintenance.py
    )
    add_test(
        NAME mcpo_analyze_cli_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_analyze_cli.py
            $<TARGET_FILE:mcp-observatory>
    )
    add_test(
        NAME mcpo_runtime_discovery_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_runtime_discovery.py
    )
    add_test(
        NAME mcpo_bulk_static_analysis_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_bulk_static_analysis.py
    )
    add_test(
        NAME mcpo_storage_v2_foundation_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage_v2_foundation.py
    )
    add_test(
        NAME mcpo_storage_v2_mvp_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage_v2_mvp.py
    )
    add_test(
        NAME mcpo_storage_v2_registry_sync_tests
        COMMAND ${Python3_EXECUTABLE}
            ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_storage_v2_registry_sync.py
    )
    add_test(
        NAME mcpo_storage_v2_prepare_script_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_storage_v2_sidecar.sh
    )
    add_test(
        NAME mcpo_storage_v2_static_script_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_storage_v2_static_batch.sh
    )
    add_test(
        NAME mcpo_storage_v2_refresh_script_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_storage_v2_registry_refresh.sh
    )
    add_test(
        NAME mcpo_storage_v2_runtime_script_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/run_storage_v2_runtime_observation.sh
    )
    add_test(
        NAME mcpo_storage_v2_verify_script_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/verify_storage_v2_sidecar.sh
    )
    add_test(
        NAME mcpo_static_analysis_installer_syntax
        COMMAND bash -n
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_static_analysis_scheduler.sh
    )
    add_test(
        NAME mcpo_static_analysis_installer_help
        COMMAND bash
            ${CMAKE_CURRENT_SOURCE_DIR}/scripts/install_static_analysis_scheduler.sh
            --help
    )
endif()
