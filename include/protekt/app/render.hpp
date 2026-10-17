#pragma once

#include <iosfwd>
#include <span>
#include <string_view>

namespace protekt::app {

///   protekt-render [--seed N] [--width N] [--height N] [--out FILE.ppm] [--gpu NAME]
///                  [--list-gpus]
///
/// Renders one frame headlessly and writes it as a binary PPM (P6), so the output can be looked
/// at without a window and diffed without a viewer. --list-gpus prints the devices the renderer
/// can see and exits 0.
///
/// Exits 0 on success, 1 when a frame or the output file could not be produced, 2 on a usage
/// error, 4 when no device could be created (with the reason on `err`). Defaults: seed 42,
/// 640x400, out "frame.ppm".
int run_render_cli(std::span<const std::string_view> args, std::ostream& out, std::ostream& err);

} // namespace protekt::app
