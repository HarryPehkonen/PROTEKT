#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace protekt::app {

///   protekt-game [--seed N] [--frames N] [--capture FILE.ppm] [--headless]
///                [--width N] [--height N] [--gpu NAME] [--ticks-per-frame N]
///
/// The game loop. With `--headless` it renders each frame offscreen through `render::Renderer`
/// and needs no display; without it, through a real window and swapchain. The loop itself does
/// not care which, which is why this is testable on a machine with no GPU.
///
/// `--capture FILE` writes the last frame as a binary PPM. Defaults: seed 42, 600 frames,
/// 1280x720, one sim tick per frame. Exits 0 on success, 1 if a frame could not be produced,
/// 2 on a usage error, 4 when no renderer or window could be created (reason on `err`).
int run_game_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

} // namespace protekt::app
