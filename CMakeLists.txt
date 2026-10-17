# PROTEKT — build.
#
# Nothing game-specific lives here beyond the shape: a core library, a test binary, and the
# single-source version. Keep it that way; the gate reads project() VERSION and expects the
# generated header below.

cmake_minimum_required(VERSION 3.20)

project(PROTEKT VERSION 0.1.0 LANGUAGES CXX)

# ================================ version (one place) ================================
# project() VERSION above is the ONLY place the number is written. The gate's `version`
# stage compares it against $CI_BUILD_DIR/generated/version.hpp, which is why the file is
# generated one directory deep rather than into a project subdirectory: the gate's default
# CI_VERSION_HEADER expects exactly this path, so a fresh clone is green with no .ci.env.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/version.hpp.in
    ${CMAKE_BINARY_DIR}/generated/version.hpp
    @ONLY
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# An unset build type silently compiles at -O0; default to Release for a game.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(NOT CMAKE_EXPORT_COMPILE_COMMANDS AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # clang-tidy reads build/compile_commands.json
endif()

# Warnings every first-party target carries. A warning is a build failure
# (CODING_STANDARDS.md rule 1); the gate also counts `warning:` lines in targets this
# does not reach.
function(protekt_warnings target)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
endfunction()

# ================================ core ================================
add_library(protekt_core STATIC
    src/version.cpp
)
target_include_directories(protekt_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
protekt_warnings(protekt_core)

# ================================ tests ================================
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(protekt_tests
        tests/test_version.cpp
    )
    target_link_libraries(protekt_tests PRIVATE protekt_core GTest::gtest_main)
    protekt_warnings(protekt_tests)

    add_test(NAME protekt_tests COMMAND protekt_tests)
endif()
