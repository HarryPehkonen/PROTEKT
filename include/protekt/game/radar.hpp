#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "protekt/game/nav_marks.hpp"
#include "protekt/game/scene_data.hpp"
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
/// The ring carries four letters, N, E, S and W, for the TRUE world directions (x east, z north, as
/// the sky and the sun define them). The panel turns with the tank, so the letters turn with it:
/// `radar_compass` takes the same yaw `radar_marks` does and is the only place they are placed.
///
/// The panel is anchored to a corner of the FRAME, not to the world, and its size follows the frame
/// width, so it is the same fraction of the screen at 1280x720 and at a full-screen resolution. It
/// sits in the bottom-left corner, and that is deliberate: the aim point is pinned to the middle
/// horizontally, so a click can never land on the radar.
namespace protekt::game {

inline constexpr int kRadarRadiusTiles = 40;
inline constexpr float kRadarMarginPixels = 24.0F;
/// The panel is BZ2's wide shallow oval: a circular ground disc seen from `kRadarElevationRadians`
/// (21 degrees) above, so its height is `kRadarAspect` (= sin 21 degrees) of its width. Both, and
/// the 0.258 of the frame it spans, were pixel-counted off BZCC screenshots.
inline constexpr float kRadarWidthFraction = 0.258F;
inline constexpr float kRadarMinPixels = 200.0F;
inline constexpr float kRadarMaxPixels = 480.0F;
inline constexpr float kRadarAspect = 0.358F;
inline constexpr float kRadarElevationRadians = 0.3665F;
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

/// The panel's bounding rectangle in the frame, with the y as the frame's y: from the top. The face
/// is the oval inscribed in it; `height` is `width * kRadarAspect`.
struct RadarPanel {
    float x{};
    float y{};
    float width{};
    float height{};
};

/// A point in panel pixels, like a mark's centre.
struct RadarPoint {
    float px{};
    float py{};
};

/// THE projection, and the only place it lives: a ground offset from the viewer in tiles, `u` to
/// the camera's right and `v` along its forward, onto the tilted plane. x is `u * s` and y is `v *
/// sin(elevation) * s` (up the panel, so subtracted), where `s` is `size / (2 * radius_tiles)`
/// pixels per tile. A point `radius_tiles` away therefore lands on the rim of the oval whatever its
/// direction. Marks, nav marks and arrows are all placed through this; the compass letters share
/// its ellipse.
[[nodiscard]] RadarPoint radar_project(const RadarPanel& panel, float u, float v,
                                       int radius_tiles = kRadarRadiusTiles) noexcept;

/// The panel for a frame of this size: bottom-left, with a margin, sized to the frame and clamped
/// so that a small window still has a usable radar and a large one does not grow it without limit.
[[nodiscard]] RadarPanel radar_panel(Extent extent) noexcept;

/// One compass letter, in panel pixels like a mark: the centre of the glyph.
struct RadarLetter {
    char glyph{};
    float px{};
    float py{};
};

/// How far in from the oval's rim a letter's centre sits, as pixels, on both axes. The glyph and
/// its plate are drawn around this point, so the plate reaches the ring and not past it (the
/// plate's corners are what set it: at the diagonals they reach furthest).
inline constexpr float kRadarLetterInsetPixels = 10.0F;

/// N, E, S, W in that order, at the ring, for a camera yaw. At yaw 0 the tank faces north, so N is
/// at the top; a quarter turn to the right puts N on the left. Never diagonals.
[[nodiscard]] std::array<RadarLetter, 4> radar_compass(float yaw_radians,
                                                       const RadarPanel& panel) noexcept;

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

/// One navigational mark on the radar, in panel pixels like `RadarMark`: the centre of its plate,
/// `half` the plate's half-size. The plate carries the mark's number.
struct RadarNav {
    float px{};
    float py{};
    float half{};
    int number{};
    bool at_rim{false}; ///< outside the radius: pinned to the rim, in the direction to turn
};

/// The nav marks for one frame, in ascending number, one per mark given -- a number that was never
/// dropped is not in `marks`, so it is not here. Inside `radius_tiles` a mark is where its tile is;
/// outside it is pinned to the rim in its direction, the way your own base is: a mark exists so you
/// can find your way back to it, and a mark that vanished when you drove away would be no use. With
/// no viewer unit there is no centre and so no marks.
[[nodiscard]] std::vector<RadarNav> radar_nav_marks(std::span<const sim::Unit> units,
                                                    sim::UnitId viewer_unit,
                                                    std::span<const NavMarks::Mark> marks,
                                                    float yaw_radians, const RadarPanel& panel,
                                                    int radius_tiles = kRadarRadiusTiles);

} // namespace protekt::game
