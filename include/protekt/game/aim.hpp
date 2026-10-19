#pragma once

#include <optional>

#include "protekt/game/scene.hpp"
#include "protekt/game/scene_data.hpp"
#include "protekt/input/input.hpp"

namespace protekt::game {

/// Where the gun is pointed: the middle of the frame, both ways.
///
/// The pointer is captured for mouse look, so there is no free cursor and no cursor position left
/// to take an aim from. The crosshair is fixed at the centre of the screen and the player aims by
/// LOOKING -- with the mouse, or with A/D. There is one aim point and everything reads it: the
/// reticle, the trigger, and what a left or right click acts on. To put the mark on something, look
/// at it.
///
/// Nothing when the extent is empty or negative (a headless frame with no framebuffer): a frame
/// with no mark also has no picks, which is what leaves those frames exactly as they were.
[[nodiscard]] std::optional<Aim> aim_from(const input::InputFrame& frame, Extent extent) noexcept;

} // namespace protekt::game
