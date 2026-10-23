#include "protekt/game/radar.hpp"

#include <algorithm>
#include <cmath>

namespace protekt::game {

namespace {

constexpr float kDotHalfFraction = 0.022F;
constexpr float kBaseHalfFraction = 0.05F;
constexpr float kViewerHalfFraction = 0.03F;
constexpr float kArrowHalfFraction = 0.045F;
/// A nav mark's plus is this half-size, of the panel: 4 px at the 230 px panel, so the mark spans
/// about 9 px and is SMALLER than a unit blip (`kDotHalfFraction`, about 10 px) -- BZ2 shows a
/// placed nav point as a tiny plus in its radar and the user asked for ours to be no bigger than
/// the tanks'. Kept clear of the rim by `kNavRimFraction`.
constexpr float kNavHalfFraction = 0.019F;
constexpr float kNavRimFraction = 0.02F;

[[nodiscard]] float tiles(const sim::Fixed& value) noexcept {
    return static_cast<float>(value.raw) / static_cast<float>(sim::Fixed::kOne);
}

} // namespace

RadarPanel radar_panel(Extent extent) noexcept {
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    // Sized from the width, so the panel keeps its place in the layout at any resolution -- but
    // never taller than the window can hold with its margin, which is what a small window is.
    const float room = std::max(height - (2.0F * kRadarMarginPixels), 32.0F);
    const float size
        = std::min(std::clamp(width * kRadarWidthFraction, kRadarMinPixels, kRadarMaxPixels),
                   room / kRadarAspect);
    const float tall = size * kRadarAspect;
    return RadarPanel{kRadarMarginPixels, height - kRadarMarginPixels - tall, size, tall};
}

RadarPoint radar_project(const RadarPanel& panel, float u, float v, int radius_tiles) noexcept {
    const float scale = panel.size / (2.0F * static_cast<float>(std::max(radius_tiles, 1)));
    return RadarPoint{(panel.size * 0.5F) + (u * scale),
                      (panel.height * 0.5F) - (v * std::sin(kRadarElevationRadians) * scale)};
}

namespace {

/// Pulls a point toward the middle of the oval until it is inside the oval whose semi-axes are the
/// panel's, each reduced by `inset`. The pull is along the ray from the middle, so a direction is
/// kept. A point already inside is left exactly where it is.
[[nodiscard]] RadarPoint pin_inside(const RadarPanel& panel, RadarPoint p, float inset) noexcept {
    const float a = std::max((panel.size * 0.5F) - inset, 1.0F);
    const float b = std::max((panel.height * 0.5F) - inset, 1.0F);
    const float x = (p.px - (panel.size * 0.5F)) / a;
    const float y = (p.py - (panel.height * 0.5F)) / b;
    const float reach = std::hypot(x, y);
    if (reach <= 1.0F) {
        return p;
    }
    return RadarPoint{(panel.size * 0.5F) + (x / reach * a),
                      (panel.height * 0.5F) + (y / reach * b)};
}

/// Pulls a square mark's centre toward the middle until all four corners of its box are on the
/// face. A fixed inset from a flat oval is not enough (the corner of a box at the end of the long
/// axis reaches past the offset curve), so this measures the corners and shrinks until they fit.
[[nodiscard]] RadarPoint fit_box(const RadarPanel& panel, RadarPoint p, float half) noexcept {
    const float middle_x = panel.size * 0.5F;
    const float middle_y = panel.height * 0.5F;
    for (int pass = 0; pass < 16; ++pass) {
        float worst = 0.0F;
        for (const float dx : {-half, half}) {
            for (const float dy : {-half, half}) {
                const float x = (p.px + dx - middle_x) / middle_x;
                const float y = (p.py + dy - middle_y) / middle_y;
                worst = std::max(worst, (x * x) + (y * y));
            }
        }
        if (worst <= 1.0F) {
            break;
        }
        const float shrink = 1.0F / std::sqrt(worst);
        p = RadarPoint{middle_x + ((p.px - middle_x) * shrink),
                       middle_y + ((p.py - middle_y) * shrink)};
    }
    return p;
}

} // namespace

std::array<RadarLetter, 4> radar_compass(float yaw_radians, const RadarPanel& panel) noexcept {
    // World north is (dx, dz) = (0, 1), east is (1, 0), pushed through the SAME forward/right the
    // marks use: forward = dx*sin + dz*cos, right = dx*cos - dz*sin. The letters ride the ellipse
    // inset from the rim, so a quarter turn is a quarter of the way round it.
    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);
    const float middle_x = panel.size * 0.5F;
    const float middle_y = panel.height * 0.5F;
    const float radius_x = std::max(middle_x - kRadarLetterInsetPixels, 0.0F);
    const float radius_y = std::max(middle_y - kRadarLetterInsetPixels, 0.0F);
    const auto at = [&](char glyph, float dx, float dz) {
        const float forward = (dx * sine) + (dz * cosine);
        const float right = (dx * cosine) - (dz * sine);
        return RadarLetter{glyph, middle_x + (right * radius_x), middle_y - (forward * radius_y)};
    };
    return {at('N', 0.0F, 1.0F), at('E', 1.0F, 0.0F), at('S', 0.0F, -1.0F), at('W', -1.0F, 0.0F)};
}

std::vector<RadarMark> radar_marks(std::span<const sim::Unit> units, sim::UnitId viewer_unit,
                                   float yaw_radians, const RadarPanel& panel, int radius_tiles) {
    std::vector<RadarMark> marks;
    if (!viewer_unit.valid() || radius_tiles <= 0 || panel.size <= 0.0F) {
        return marks;
    }
    const sim::Unit* viewer = nullptr;
    for (const sim::Unit& unit : units) {
        if (unit.id == viewer_unit) {
            viewer = &unit;
        }
    }
    if (viewer == nullptr) {
        return marks;
    }

    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);

    // You: at the middle, always pointing up, because the panel turns with you.
    const RadarPoint middle = radar_project(panel, 0.0F, 0.0F, radius_tiles);
    marks.push_back(RadarMark{middle.px, middle.py, panel.size * kViewerHalfFraction, 0.0F,
                              RadarShape::Triangle, RadarTone::You});

    for (const sim::Unit& unit : units) {
        if (!unit.alive || unit.id == viewer->id) {
            continue;
        }
        const float dx = tiles(unit.x) - tiles(viewer->x);
        const float dz = tiles(unit.y) - tiles(viewer->y);
        const float distance = std::hypot(dx, dz);
        const bool in_range = distance <= static_cast<float>(radius_tiles);
        const bool base = unit.kind == sim::UnitKind::Factory && unit.side == viewer->side;
        if (!in_range && !base) {
            continue; // outside the radius the player knows nothing, and this is where that is kept
        }
        // The camera's own frame, from the same yaw the projection is built with: forward at yaw 0
        // is +Z, and right is (cos, -sin) -- exactly `math::basis_from`'s horizontal part.
        const float forward = (dx * sine) + (dz * cosine);
        const float right = (dx * cosine) - (dz * sine);
        if (in_range) {
            // Where the projection puts it, pulled in just far enough that its box stays on the
            // face: a mark at the radius is ON the rim, and half of it would be outside the oval.
            const float half
                = panel.size
                  * (unit.kind == sim::UnitKind::Factory ? kBaseHalfFraction : kDotHalfFraction);
            const RadarPoint at
                = fit_box(panel, radar_project(panel, right, forward, radius_tiles), half);
            marks.push_back(
                RadarMark{at.px, at.py, half, 0.0F, base ? RadarShape::Hollow : RadarShape::Square,
                          unit.side == viewer->side ? RadarTone::Yours : RadarTone::Hostile});
            continue;
        }
        // Your own base, out of range: a mark at the rim, pointing the way to turn. The arrow
        // points along the GROUND bearing (the way to turn), though it sits where that bearing
        // meets the oval.
        const float length = std::max(distance, 1.0e-3F);
        const float ground_x = right / length;
        const float ground_y = forward / length;
        const float half = panel.size * kArrowHalfFraction;
        const RadarPoint at
            = fit_box(panel,
                      radar_project(panel, ground_x * static_cast<float>(radius_tiles),
                                    ground_y * static_cast<float>(radius_tiles), radius_tiles),
                      half);
        marks.push_back(RadarMark{at.px, at.py, half, std::atan2(ground_x, ground_y),
                                  RadarShape::Arrow, RadarTone::Yours});
    }
    return marks;
}

std::vector<RadarNav> radar_nav_marks(std::span<const sim::Unit> units, sim::UnitId viewer_unit,
                                      std::span<const NavMarks::Mark> marks, float yaw_radians,
                                      const RadarPanel& panel, int radius_tiles) {
    std::vector<RadarNav> shown;
    if (!viewer_unit.valid() || radius_tiles <= 0 || panel.size <= 0.0F) {
        return shown;
    }
    const sim::Unit* viewer = nullptr;
    for (const sim::Unit& unit : units) {
        if (unit.id == viewer_unit) {
            viewer = &unit;
        }
    }
    if (viewer == nullptr) {
        return shown;
    }
    const float half = panel.size * kNavHalfFraction;
    // The plate stays wholly on the face: its centre never goes past the oval inset by this much.
    const float inset = (kNavRimFraction * panel.size) + (1.5F * half);
    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);
    for (const NavMarks::Mark& mark : marks) {
        // The middle of the tile, in the same tile units as a unit's position.
        const float dx = (static_cast<float>(mark.tile_x) + 0.5F) - tiles(viewer->x);
        const float dz = (static_cast<float>(mark.tile_y) + 0.5F) - tiles(viewer->y);
        const float distance = std::hypot(dx, dz);
        const float forward = (dx * sine) + (dz * cosine);
        const float right = (dx * cosine) - (dz * sine);
        const bool in_range = distance <= static_cast<float>(radius_tiles);
        // Out of range the mark is first put at the radius in its direction -- the rim -- and
        // either way the pin then draws it in to where the whole plate is on the face.
        float u = right;
        float v = forward;
        if (!in_range) {
            const float scale = static_cast<float>(radius_tiles) / std::max(distance, 1.0e-3F);
            u *= scale;
            v *= scale;
        }
        const RadarPoint at = pin_inside(panel, radar_project(panel, u, v, radius_tiles), inset);
        shown.push_back(RadarNav{at.px, at.py, half, mark.number, !in_range});
    }
    return shown;
}

} // namespace protekt::game
