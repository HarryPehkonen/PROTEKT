#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

#include "protekt/game/radar.hpp"

namespace protekt::app {

/// How far the world view draws, in tiles about the camera: exactly as far as the tank's radar
/// reaches, and no further. You see what your radar sees; the reach cull and the pick rule
/// (`hidden_by_reach`, `outside_drawn_radius`) then decide the band at the edge -- and with these
/// two numbers equal, that band is the corners of the box, which is the only place the square view
/// and the round radar disagree. The fog ends at the meshed rim, so it follows this number and
/// needs none of its own.
///
/// It is not free. Measured on the Pi back to back at 320x180 (software renderer, 400 frames, one
/// run each): 24 tiles = 37.1 ms/frame (27 fps), 40 tiles = 56.0 ms/frame (18 fps), against the
/// sim's 30 fps lock at 33.3 ms. The cost grows with the meshed area, (2*radius+1)^2 tiles. 40 is
/// the game's number because it makes the radar reach the binding rule rather than the mesh;
/// dial it here if frame rate matters more. `SceneOptions::radius_tiles` keeps its own default
/// for tests and other callers.
inline constexpr int kWorldViewRadiusTiles = game::kRadarRadiusTiles;

///   protekt-game [--seed N] [--frames N] [--capture FILE.ppm] [--headless]
///                [--width N] [--height N] [--gpu NAME] [--ticks-per-frame N]
///                [--scenario FILE.toml] [--log-spawns]
///
/// The game loop. With `--headless` it renders each frame offscreen through `render::Renderer`
/// and needs no display; without it, through a real window and swapchain. The loop itself does
/// not care which, which is why this is testable on a machine with no GPU.
///
/// `--capture FILE` writes the last frame as a binary PPM. Defaults: seed 42, 600 frames,
/// 1280x720, one sim tick per frame. Exits 0 on success, 1 if a frame could not be produced,
/// 2 on a usage error, 4 when no renderer or window could be created (reason on `err`).
///
/// `--scenario FILE` plays the composition in a TOML file (see scenario_toml.hpp) instead of the
/// built-in one. There is no search path: with no flag the built-in is played, and a file that
/// cannot be read or parsed refuses to start (exit 2) with the parser's message on `err`.
int run_game_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

} // namespace protekt::app
