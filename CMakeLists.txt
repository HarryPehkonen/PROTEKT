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
    src/math/math.cpp
    src/rng.cpp
    src/world/world.cpp
    src/world/sample.cpp
    src/sim/fixed.cpp
    src/sim/sim.cpp
    src/sim/projectile.cpp
    src/sim/scrap.cpp
    src/sim/scenario.cpp
    src/sim/replay.cpp
    src/sim/observer.cpp
    src/ai/scripted_commander.cpp
    src/ai/utility.cpp
)
target_include_directories(protekt_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
protekt_warnings(protekt_core)

# ================================ command ================================
# The human's side of the order interface: the selection and the verbs that turn a click into
# orders. Pure logic -- no window, no GPU, no clock -- so every part of it
# runs in the gate on a machine with no display.
add_library(protekt_cmd STATIC
    src/cmd/selection.cpp
    src/cmd/command.cpp
)
target_link_libraries(protekt_cmd PUBLIC protekt_core)
protekt_warnings(protekt_cmd)

# ================================ audio ================================
# Sound made of arithmetic: synthesis and mixing, pure functions of their arguments. It depends on
# nothing in the project (the sim does not know it exists, and it does not know the sim), so it
# is tested on a machine with no sound hardware like everything else.
add_library(protekt_audio STATIC
    src/audio/synth.cpp
    src/audio/mixer.cpp
)
target_include_directories(protekt_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
protekt_warnings(protekt_audio)

# ================================ game ================================
# The game's own logic -- the world as seen, and the player as a commander -- is PURE, has no
# window and no Vulkan in it, and is therefore tested on the gate box with everything else.
# The atlas manifest is committed data (assets/atlas_manifest.txt) and is compiled INTO the binary,
# like the shaders: the game reads no data file at run time, and a configure re-runs when it changes.
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/assets/atlas_manifest.txt PROTEKT_ATLAS_MANIFEST_TEXT)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS assets/atlas_manifest.txt)
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/atlas_manifest.hpp.in
    ${CMAKE_BINARY_DIR}/generated/atlas_manifest.hpp
    @ONLY
)
add_library(protekt_game STATIC
    src/game/aim.cpp
    src/game/attack_aim.cpp
    src/game/atlas.cpp
    src/game/png.cpp
    src/game/scene.cpp
    src/game/player.cpp
    src/game/pick.cpp
    src/game/interaction.cpp
    src/game/session.cpp
    src/game/navigate.cpp
    src/game/radar.cpp
    src/game/nav_marks.cpp
    src/game/row.cpp
    src/game/menu.cpp
    src/game/targets.cpp
    src/game/world_map.cpp
    src/game/sounds.cpp
    src/game/weapon.cpp
)
target_include_directories(protekt_game PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_BINARY_DIR}/generated
)
# stb (third_party/stb) is public domain / MIT and is vendored whole; SYSTEM keeps its warnings out
# of our -Werror, and only src/game/png.cpp includes it.
target_include_directories(protekt_game SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/stb)
target_link_libraries(protekt_game PUBLIC protekt_core protekt_cmd protekt_audio)
protekt_warnings(protekt_game)
# ================================ render ================================
# Vulkan is OPTIONAL at build time: a machine with no loader or no shader compiler still builds
# the whole simulation, its tests, and both text CLIs. The renderer's own tests are then ABSENT
# rather than silently passing, and configure says so out loud.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    # Fedora and its rebuilds ship the loader as libvulkan.so.1 with no `libvulkan.so` development
    # symlink and no vulkan.pc unless a -devel package is installed, so CMake's own finder gives up
    # on a machine that runs Vulkan perfectly well (measured on nobara-pc: headers present, loader
    # present, Vulkan_FOUND false). Look for the versioned loader directly instead of making the
    # owner of the machine install a package to satisfy a finder.
    find_path(PROTEKT_VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h)
    set(PROTEKT_VULKAN_LOADER "")
    foreach(candidate /usr/lib64/libvulkan.so.1 /usr/lib/libvulkan.so.1
                      /usr/lib/x86_64-linux-gnu/libvulkan.so.1)
        if(PROTEKT_VULKAN_LOADER STREQUAL "" AND EXISTS ${candidate})
            set(PROTEKT_VULKAN_LOADER ${candidate})
        endif()
    endforeach()
    if(PROTEKT_VULKAN_INCLUDE_DIR AND PROTEKT_VULKAN_LOADER)
        set(Vulkan_FOUND TRUE)
        set(Vulkan_INCLUDE_DIRS ${PROTEKT_VULKAN_INCLUDE_DIR})
        set(Vulkan_LIBRARIES ${PROTEKT_VULKAN_LOADER})
        if(NOT TARGET Vulkan::Vulkan)
            add_library(Vulkan::Vulkan UNKNOWN IMPORTED)
            set_target_properties(Vulkan::Vulkan PROPERTIES
                IMPORTED_LOCATION ${PROTEKT_VULKAN_LOADER}
                INTERFACE_INCLUDE_DIRECTORIES ${PROTEKT_VULKAN_INCLUDE_DIR})
        endif()
        message(STATUS "PROTEKT: Vulkan found by hand (${PROTEKT_VULKAN_LOADER}) -- the loader has no .so development symlink here")
    endif()
endif()
find_program(GLSLANG_VALIDATOR glslangValidator)
# SDL3 is for the window and the keyboard, nothing else. No window without it, and the game still
# builds and runs headless.
find_package(SDL3 QUIET)
if(NOT SDL3_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL3 IMPORTED_TARGET sdl3)
    endif()
endif()
if(Vulkan_FOUND AND GLSLANG_VALIDATOR)
    set(PROTEKT_RENDER ON)
else()
    set(PROTEKT_RENDER OFF)
    message(STATUS "PROTEKT: renderer DISABLED (Vulkan_FOUND=${Vulkan_FOUND}, glslangValidator=${GLSLANG_VALIDATOR})")
endif()

if(PROTEKT_RENDER AND (SDL3_FOUND OR TARGET PkgConfig::SDL3))
    set(PROTEKT_WINDOW ON)
else()
    set(PROTEKT_WINDOW OFF)
    message(STATUS "PROTEKT: window DISABLED (SDL3 ${SDL3_FOUND}); the game runs --headless only")
endif()

if(PROTEKT_RENDER)
    set(PROTEKT_SHADER_DIR ${CMAKE_BINARY_DIR}/generated/shaders)
    # Shaders are compiled to SPIR-V headers at build time and linked in, so the binary needs no
    # data files beside it and cannot pick up a stale .spv from somebody's home directory.
    add_custom_command(
        OUTPUT ${PROTEKT_SHADER_DIR}/scene_vert.h ${PROTEKT_SHADER_DIR}/scene_frag.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${PROTEKT_SHADER_DIR}
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 --vn scene_vert_spv
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.vert -o ${PROTEKT_SHADER_DIR}/scene_vert.h
        COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.3 --vn scene_frag_spv
                ${CMAKE_CURRENT_SOURCE_DIR}/shaders/scene.frag -o ${PROTEKT_SHADER_DIR}/scene_frag.h
        DEPENDS shaders/scene.vert shaders/scene.frag
        COMMENT "Compiling shaders to SPIR-V")
    add_custom_target(protekt_shaders
        DEPENDS ${PROTEKT_SHADER_DIR}/scene_vert.h ${PROTEKT_SHADER_DIR}/scene_frag.h)

    set(PROTEKT_RENDER_SOURCES
        src/render/renderer.cpp
    )
    if(PROTEKT_WINDOW)
        list(APPEND PROTEKT_RENDER_SOURCES src/render/window.cpp)
    endif()
    add_library(protekt_render STATIC ${PROTEKT_RENDER_SOURCES})
    target_include_directories(protekt_render PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_BINARY_DIR}/generated
        ${PROTEKT_SHADER_DIR}
    )
    target_link_libraries(protekt_render PUBLIC protekt_core Vulkan::Vulkan)
    if(PROTEKT_WINDOW)
        if(SDL3_FOUND)
            target_link_libraries(protekt_render PUBLIC SDL3::SDL3)
        else()
            target_link_libraries(protekt_render PUBLIC PkgConfig::SDL3)
        endif()
        target_compile_definitions(protekt_render PUBLIC PROTEKT_HAVE_WINDOW=1)
    endif()
    protekt_warnings(protekt_render)
    add_dependencies(protekt_render protekt_shaders)
endif()

# ================================ app ================================
# The top of the graph: the command-line front ends' shared code, the game loop and the audio
# device. It is the ONLY library that may see the window and the renderer; nothing below it links
# protekt_render or sees PROTEKT_HAVE_WINDOW. Defined AFTER the render section because
# PROTEKT_WINDOW is computed there -- wiring it earlier made the condition always false and silently
# built a binary with no window in it, which `nm` showed.
set(PROTEKT_APP_SOURCES src/app/cli.cpp src/app/scenario_toml.cpp src/app/audio.cpp src/app/audio_pipeline.cpp src/app/player_view.cpp src/app/game_presenter.cpp src/app/game.cpp)
if(PROTEKT_RENDER)
    list(APPEND PROTEKT_APP_SOURCES src/app/render.cpp)
endif()
add_library(protekt_app STATIC ${PROTEKT_APP_SOURCES})
target_link_libraries(protekt_app PUBLIC protekt_game protekt_cmd protekt_audio)
protekt_warnings(protekt_app)
# The sound device is SDL3's, and independent of the window: a build with SDL but no Vulkan can
# still be heard. Without SDL the device is a stub that says so and the game is silent. The
# sound code lives in protekt_app, so the link belongs there: protekt_game itself has no
# SDL include and stays headless.
if(SDL3_FOUND OR TARGET PkgConfig::SDL3)
    if(SDL3_FOUND)
        target_link_libraries(protekt_app PUBLIC SDL3::SDL3)
    else()
        target_link_libraries(protekt_app PUBLIC PkgConfig::SDL3)
    endif()
    target_compile_definitions(protekt_app PUBLIC PROTEKT_HAVE_SDL_AUDIO=1)
endif()
if(PROTEKT_RENDER)
    target_link_libraries(protekt_app PUBLIC protekt_render)
endif()
if(PROTEKT_WINDOW)
    # game.cpp drives a window when there is one, so every target that compiles it must see the
    # same define; they all get it by linking protekt_app.
    target_compile_definitions(protekt_app PUBLIC PROTEKT_HAVE_WINDOW=1)
endif()


# ================================ tests ================================
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    enable_testing()
    find_package(GTest REQUIRED)

    add_executable(protekt_tests
        tests/test_version.cpp
        tests/test_rng.cpp
        tests/test_world.cpp
        tests/test_world_sample.cpp
        tests/test_world_wide.cpp
        tests/test_fixed.cpp
        tests/test_math.cpp
        tests/test_aim.cpp
        tests/test_steering.cpp
        tests/test_focus_fire.cpp
        tests/test_follow.cpp
        tests/test_mount.cpp
        tests/test_spec.cpp
        tests/test_mortar.cpp
        tests/test_bouncer.cpp
        tests/test_shotgun.cpp
        tests/test_rocket_weapon.cpp
        tests/test_manual_fire.cpp
        tests/test_roster.cpp
        tests/test_hover_drive.cpp
        tests/test_friendly_fire.cpp
        tests/test_sim.cpp
        tests/test_driven.cpp
        tests/test_projectile.cpp
        tests/test_scrap.cpp
        tests/test_recycle.cpp
        tests/test_scavenger.cpp
        tests/test_ai_aim.cpp
        tests/test_scenario.cpp
        tests/test_scenario_data.cpp
        tests/test_replay.cpp
        tests/test_observer.cpp
        tests/test_commander.cpp
        tests/test_cli.cpp
        tests/test_args.cpp
        tests/test_fuzz_entry.cpp
        fuzz/replay_entry.cpp
    )
    target_link_libraries(protekt_tests PRIVATE protekt_core GTest::gtest_main)
    protekt_warnings(protekt_tests)
    target_compile_definitions(protekt_tests
        PRIVATE PROTEKT_CORPUS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fuzz/corpus/replay"
                PROTEKT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_sources(protekt_tests PRIVATE tests/test_scene.cpp tests/test_radar.cpp tests/test_player.cpp
                                        tests/test_game_cli.cpp tests/test_selection.cpp
                                        tests/test_command.cpp
                                        tests/test_pick.cpp tests/test_input.cpp
                                        tests/test_interaction.cpp tests/test_bindings.cpp
                                        tests/test_weapon_lineup.cpp
                                        tests/test_session.cpp tests/test_navigate.cpp
                                        tests/test_utility.cpp tests/test_time_accumulator.cpp
                                        tests/test_audio_pipeline.cpp tests/test_wake_log.cpp tests/test_m5.cpp tests/test_nav_marks.cpp tests/test_send_to_mark.cpp tests/test_row_slots.cpp tests/test_row_move.cpp tests/test_targets.cpp tests/test_world_map.cpp tests/test_player_view.cpp tests/test_game_presenter.cpp
                                        tests/test_menu.cpp tests/test_utility_battle.cpp tests/test_building_keys.cpp tests/test_attack_offer.cpp
                                        tests/test_layering.cpp
                                        tests/test_navigation.cpp tests/test_routing.cpp
                                        tests/test_synth.cpp tests/test_mixer.cpp
                                        tests/test_sounds.cpp tests/test_audio_out.cpp
                                        tests/test_pixels.cpp tests/test_atlas.cpp
                                        tests/test_atlas_files.cpp tests/test_ground_variants.cpp
                                        tests/test_map_cli.cpp
                                        tests/test_fog.cpp)
    target_link_libraries(protekt_tests PRIVATE protekt_app)
    if(PROTEKT_RENDER)
        target_sources(protekt_tests PRIVATE tests/test_render.cpp)
        target_link_libraries(protekt_tests PRIVATE protekt_render)
        target_compile_definitions(protekt_tests PRIVATE PROTEKT_HAVE_VULKAN=1)
        add_dependencies(protekt_tests protekt_shaders)
    endif()

    add_test(NAME protekt_tests COMMAND protekt_tests)

    # The map CLI exists so that determinism can be checked ACROSS processes, which no unit
    # test can do: run it twice with the same seed and the bytes must match.
    add_executable(protekt-map src/app/map_cli.cpp)
    target_link_libraries(protekt-map PRIVATE protekt_app)
    protekt_warnings(protekt-map)
    add_dependencies(protekt_tests protekt-map)
    target_compile_definitions(protekt_tests PRIVATE PROTEKT_MAP="$<TARGET_FILE:protekt-map>")

    # The sim CLI: the same battle the tests run, printed for a human.
    add_executable(protekt-sim src/app/sim_cli.cpp)
    target_link_libraries(protekt-sim PRIVATE protekt_app)
    protekt_warnings(protekt-sim)

    # The commander measurement: "better than scripted" as a number, reproducible by anyone, with a
    # placebo arm, because the obvious metric (units lost) is won by a commander that does nothing.
    add_executable(protekt-measure tools/measure_commanders.cpp)
    target_link_libraries(protekt-measure PRIVATE protekt_core)
    protekt_warnings(protekt-measure)

    # The radar terrain wireframe's frame cost (RADAR3D-2): the mesh alone at both grid steps, and
    # the whole frame with it on and off. The brief asked for this measurement before choosing the
    # step, so the probe stays in the repo with the number it produced.
    add_executable(protekt-meshcost tools/meshcost.cpp)
    target_link_libraries(protekt-meshcost PRIVATE protekt_game protekt_core)
    protekt_warnings(protekt-meshcost)

    # Writes assets/atlas.png from the in-memory atlas builder (the single source of the tiles).
    add_executable(protekt-atlas-gen tools/atlas_gen.cpp)
    target_link_libraries(protekt-atlas-gen PRIVATE protekt_app)
    protekt_warnings(protekt-atlas-gen)
    add_dependencies(protekt_tests protekt-atlas-gen)
    target_compile_definitions(protekt_tests
        PRIVATE PROTEKT_ATLAS_GEN="$<TARGET_FILE:protekt-atlas-gen>")

    # The game itself: a window if SDL3 is there, an offscreen buffer if not.
    add_executable(protekt-game src/app/game_cli.cpp)
    target_link_libraries(protekt-game PRIVATE protekt_app)
    protekt_warnings(protekt-game)
    if(PROTEKT_RENDER)
        add_dependencies(protekt-game protekt_shaders)
    endif()

    # Renders one frame to a PPM: the headless half of the game's eyes.
    if(PROTEKT_RENDER)
        add_executable(protekt-render src/app/render_cli.cpp)
        target_link_libraries(protekt-render PRIVATE protekt_app)
        protekt_warnings(protekt-render)
    endif()

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
