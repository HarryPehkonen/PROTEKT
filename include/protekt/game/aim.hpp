#pragma once

#include <optional>

#include "protekt/game/scene.hpp"
#include "protekt/input/input.hpp"
#include "protekt/render/renderer.hpp"

namespace protekt::game {

/// Where the gun is pointed: horizontally the middle of the frame, vertically wherever the mouse
/// is.
///
/// The tank's gun is on its centre line and the player TURNS with A/D, so the middle of the screen
/// is the only place a shot can come from; the mouse adds elevation within that. A mark allowed
/// anywhere would promise that a click far off to the side selects the unit there, which the shot
/// could not reach -- so there is one aim point, and everything reads it: the reticle, the trigger,
/// and what a left or right click acts on. To put the mark on something, turn the tank until it is
/// there.
///
/// Nothing when no pointer has ever been seen (headless, or `--autodrive`): a frame with no mark
/// also has no picks, which is what leaves those frames exactly as they were. The vertical is
/// clamped to the frame, because a click cannot land outside it and the mark must not pretend
/// otherwise.
[[nodiscard]] std::optional<Aim> aim_from(const input::InputFrame& frame,
                                          render::Extent extent) noexcept;

} // namespace protekt::game
