#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "protekt/render/renderer.hpp"
#include "protekt/sim/sim.hpp"

/// The radar: what is near the tank you are sitting in, and nothing else.
///
/// A RADAR, NOT A MAP. It has a radius in tiles and shows only what is inside that radius, so a
/// distant hostile is not on it, not hinted at and not an arrow: outside the radius the player
/// knows nothing, which is the whole reason the radius exists. A whole-world view would reveal
/// every enemy asset in the world, and there is no fog to hide behind in a game with one player.
///
/// It is HEADING-UP: the top of the panel is always where the tank is looking, so a mark's
/// direction is a direction to turn, with no mental arithmetic in the middle of a firefight.
/// Forward and right are taken from the same camera yaw the projection uses, so the radar cannot
/// disagree with the frame.
///
/// There is no compass tick: there is nothing to correlate a bearing with yet. When there is a map,
/// a tick comes with it.
///
/// The panel is anchored to a corner of the FRAME, not to the world, and its size follows the frame
/// width, so it is the same fraction of the screen at 1280x720 and at a full-screen resolution. It
/// sits in the bottom-left corner, and that is deliberate: the aim point is pinned to the middle
/// horizontally, so a click can never land on the radar.
namespace protekt::game {

inline constexpr int kRadarRadiusTiles = 40;
inline constexpr float kRadarMarginPixels = 24.0F;
inline constexpr float kRadarWidthFraction = 0.18F;
inline constexpr float kRadarMinPixels = 140.0F;
inline constexpr float kRadarMaxPixels = 320.0F;
/// The light border drawn just outside the panel's face, in pixels. Exported because it is the
/// panel's outermost edge: anything drawn for the radar lies within the panel's rectangle widened
/// by this much, and that is a claim a test can hold the drawing to.
inline constexpr float kRadarBorderPixels = 3.0F;

/// What a mark means, which is what decides its colour. Two tones only: everything of yours looks
/// alike, everything hostile looks alike, because that is the one decision the player is making.
enum class RadarTone : std::uint8_t { You = 0, Yours = 1, Hostile = 2 };

/// A mark's shape. Shape carries meaning as well as colour, so your base is not just a bigger dot.
enum class RadarShape : std::uint8_t {
    Square = 0,   ///< a unit
    Hollow = 1,   ///< your base in range: an outline, which reads at a glance
    Triangle = 2, ///< you, at the centre, always pointing up
    Arrow = 3     ///< your base out of range: a mark at the rim pointing the way to turn
};

/// One thing the radar draws, in PANEL pixels: from the panel's left, from its TOP, like the frame.
/// `angle` is in radians, 0 pointing at the panel's top and increasing clockwise, for the shapes
/// that point.
struct RadarMark {
    float px{};
    float py{};
    float half{}; ///< half-size in pixels
    float angle{};
    RadarShape shape{RadarShape::Square};
    RadarTone tone{RadarTone::Yours};
};

/// The panel's rectangle in the frame, with the y as the frame's y: from the top.
struct RadarPanel {
    float x{};
    float y{};
    float size{};
};

/// The panel for a frame of this size: bottom-left, with a margin, sized to the frame and clamped
/// so that a small window still has a usable radar and a large one does not grow it without limit.
[[nodiscard]] RadarPanel radar_panel(render::Extent extent) noexcept;

/// The marks for one frame, in panel pixels.
///
/// The centre is the unit named by `viewer_unit`; with no such unit in `units` there are no marks
/// at all -- there is nothing to be the centre of, and a panel with no centre would be a lie about
/// where the tank is. A DEAD viewer is still the centre: death leaves a spectator, not a blank.
///
/// Everything alive inside `radius_tiles` is marked, yours and theirs. Everything outside it is
/// dropped, EXCEPT your own factories, which become an arrow at the rim: you are never lost about
/// your own base. A hostile outside the radius gets nothing.
[[nodiscard]] std::vector<RadarMark> radar_marks(std::span<const sim::Unit> units,
                                                 sim::UnitId viewer_unit, float yaw_radians,
                                                 const RadarPanel& panel,
                                                 int radius_tiles = kRadarRadiusTiles);

} // namespace protekt::game
