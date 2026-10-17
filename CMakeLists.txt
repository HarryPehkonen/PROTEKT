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
    src/rng.cpp
    src/world/world.cpp
    src/world/sample.cpp
    src/sim/fixed.cpp
    src/sim/sim.cpp
    src/sim/scenario.cpp
    src/sim/replay.cpp
    src/sim/observer.cpp
    src/ai/scripted_commander.cpp
    src/app/cli.cpp
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
        tests/test_rng.cpp
        tests/test_world.cpp
        tests/test_world_sample.cpp
        tests/test_fixed.cpp
        tests/test_sim.cpp
        tests/test_scenario.cpp
        tests/test_replay.cpp
        tests/test_observer.cpp
        tests/test_commander.cpp
        tests/test_cli.cpp
        tests/test_fuzz_entry.cpp
        fuzz/replay_entry.cpp
    )
    target_link_libraries(protekt_tests PRIVATE protekt_core GTest::gtest_main)
    protekt_warnings(protekt_tests)
    target_compile_definitions(protekt_tests
        PRIVATE PROTEKT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/replay")

    add_test(NAME protekt_tests COMMAND protekt_tests)

    # The map CLI exists so that determinism can be checked ACROSS processes, which no unit
    # test can do: run it twice with the same seed and the bytes must match.
    add_executable(protekt-map src/app/map_cli.cpp)
    target_link_libraries(protekt-map PRIVATE protekt_core)
    protekt_warnings(protekt-map)

    # The sim CLI: the same battle the tests run, printed for a human.
    add_executable(protekt-sim src/app/sim_cli.cpp)
    target_link_libraries(protekt-sim PRIVATE protekt_core)
    protekt_warnings(protekt-sim)

    # libFuzzer on the replay parser. The suite already feeds the SAME entry point the corpus and
    # generated inputs on every gate run, so this target is for depth, not for coverage.
    option(PROTEKT_BUILD_FUZZING "Build the libFuzzer targets" OFF)
    if(PROTEKT_BUILD_FUZZING)
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
            message(FATAL_ERROR "PROTEKT_BUILD_FUZZING requires clang")
        endif()
        add_executable(fuzz_replay fuzz/replay_entry.cpp)
        target_link_libraries(fuzz_replay PRIVATE protekt_core)
        protekt_warnings(fuzz_replay)
        target_compile_options(fuzz_replay PRIVATE -fsanitize=fuzzer,address,undefined
                                                    -fno-omit-frame-pointer)
        target_link_options(fuzz_replay PRIVATE -fsanitize=fuzzer,address,undefined)
    endif()
endif()