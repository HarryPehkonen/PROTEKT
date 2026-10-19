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

} // namespace protekt::game
