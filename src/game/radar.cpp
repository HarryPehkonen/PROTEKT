#include "protekt/game/radar.hpp"

#include <algorithm>
#include <cmath>

namespace protekt::game {

namespace {

/// The rim is left clear, so an edge mark cannot be clipped by the panel's own border.
constexpr float kPanelInsetFraction = 0.06F;
constexpr float kDotHalfFraction = 0.022F;
constexpr float kBaseHalfFraction = 0.05F;
constexpr float kViewerHalfFraction = 0.03F;
constexpr float kArrowHalfFraction = 0.045F;
constexpr float kArrowInsetFraction = 0.055F;
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
    // never larger than the window can hold with its margin, which is what a small window is.
    const float room = std::max(height - (2.0F * kRadarMarginPixels), 32.0F);
    const float size
        = std::min(std::clamp(width * kRadarWidthFraction, kRadarMinPixels, kRadarMaxPixels), room);
    return RadarPanel{kRadarMarginPixels, height - kRadarMarginPixels - size, size};
}

std::array<RadarLetter, 4> radar_compass(float yaw_radians, const RadarPanel& panel) noexcept {
    // World north is (dx, dz) = (0, 1), east is (1, 0), pushed through the SAME forward/right the
    // marks use: forward = dx*sin + dz*cos, right = dx*cos - dz*sin.
    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);
    const float middle = panel.size * 0.5F;
    const float radius = std::max(middle - kRadarLetterInsetPixels, 0.0F);
    const auto at = [&](char glyph, float dx, float dz) {
        const float forward = (dx * sine) + (dz * cosine);
        const float right = (dx * cosine) - (dz * sine);
        return RadarLetter{glyph, middle + (right * radius), middle - (forward * radius)};
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

    const float middle = panel.size * 0.5F;
    const float pixels_per_tile
        = (middle - (panel.size * kPanelInsetFraction)) / static_cast<float>(radius_tiles);
    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);

    // You: at the middle, always pointing up, because the panel turns with you.
    marks.push_back(RadarMark{middle, middle, panel.size * kViewerHalfFraction, 0.0F,
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
            marks.push_back(RadarMark{
                middle + (right * pixels_per_tile), middle - (forward * pixels_per_tile),
                panel.size
                    * (unit.kind == sim::UnitKind::Factory ? kBaseHalfFraction : kDotHalfFraction),
                0.0F, base ? RadarShape::Hollow : RadarShape::Square,
                unit.side == viewer->side ? RadarTone::Yours : RadarTone::Hostile});
            continue;
        }
        // Your own base, out of range: a mark at the rim, pointing the way to turn.
        const float length = std::max(distance, 1.0e-3F);
        const float panel_x = right / length;
        const float panel_y = forward / length;
        const float rim = middle - (panel.size * kArrowInsetFraction);
        marks.push_back(RadarMark{middle + (panel_x * rim), middle - (panel_y * rim),
                                  panel.size * kArrowHalfFraction, std::atan2(panel_x, panel_y),
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
    const float middle = panel.size * 0.5F;
    const float half = panel.size * kNavHalfFraction;
    // The plate stays wholly on the disc: its centre never goes past this radius.
    const float limit = middle - (kNavRimFraction * panel.size) - (1.5F * half);
    const float pixels_per_tile
        = (middle - (panel.size * kPanelInsetFraction)) / static_cast<float>(radius_tiles);
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
        float x = right * pixels_per_tile;
        float y = forward * pixels_per_tile;
        const float reach = std::hypot(x, y);
        if (!in_range) {
            const float length = std::max(distance, 1.0e-3F);
            x = right / length * limit;
            y = forward / length * limit;
        } else if (reach > limit) {
            x *= limit / reach;
            y *= limit / reach;
        }
        shown.push_back(RadarNav{middle + x, middle - y, half, mark.number, !in_range});
    }
    return shown;
}

} // namespace protekt::game
