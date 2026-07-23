cmake_minimum_required(VERSION 3.24)
project(mcp_observatory VERSION 0.1.0 LANGUAGES CXX)

option(MCPO_BUILD_TESTS "Build focused tests" ON)

add_library(mcpo_core STATIC
    src/target_manifest.cpp
)
target_include_directories(mcpo_core PUBLIC include)
target_compile_features(mcpo_core PUBLIC cxx_std_20)
target_compile_options(mcpo_core PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>
)

add_executable(mcp-observatory src/main.cpp)
target_link_libraries(mcp-observatory PRIVATE mcpo_core)
target_compile_features(mcp-observatory PRIVATE cxx_std_20)

if(MCPO_BUILD_TESTS)
    enable_testing()
    add_executable(mcpo_tests tests/test_main.cpp)
    target_link_libraries(mcpo_tests PRIVATE mcpo_core)
    target_compile_features(mcpo_tests PRIVATE cxx_std_20)
    add_test(NAME mcpo_tests COMMAND mcpo_tests)
endif()
