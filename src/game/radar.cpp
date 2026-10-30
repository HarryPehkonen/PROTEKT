#include "protekt/game/radar.hpp"

#include "protekt/game/scene.hpp" // kMetresPerTile and height_metres: what a metre is, in tiles

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

/// A tile corner's height in metres, from the cache when there is one for this seed. The same
/// memo-or-world choice the terrain mesh makes (`cached_height_metres` in scene.cpp), and the same
/// answer either way: the cache's height units times `kMetresPerHeightUnit` IS `height_metres`.
[[nodiscard]] float corner_height_metres(const TerrainCache* terrain, std::uint64_t seed,
                                         int tile_x, int tile_y) noexcept {
    if (terrain != nullptr && terrain->seed() == seed) {
        return static_cast<float>(terrain->height_units(tile_x, tile_y)) * kMetresPerHeightUnit;
    }
    return height_metres(seed, tile_x, tile_y);
}

} // namespace

int viewer_radar_radius(const sim::Sim& sim, sim::UnitId viewer) noexcept {
    const sim::Unit* unit = sim.find(viewer);
    const bool scout = unit != nullptr && unit->alive && unit->kind == sim::UnitKind::Scout;
    return scout ? kScoutRadarRadiusTiles : kRadarRadiusTiles;
}

RadarPanel radar_panel(Extent extent) noexcept {
    const float width = static_cast<float>(extent.width);
    const float height = static_cast<float>(extent.height);
    // Sized from the width, so the panel keeps its place in the layout at any resolution -- but
    // never taller than the window can hold with its margin, which is what a small window is.
    const float room = std::max(height - (2.0F * kPanelMarginPixels), 32.0F);
    const float size
        = std::min(std::clamp(width * kRadarWidthFraction, kRadarMinPixels, kRadarMaxPixels),
                   room / kRadarAspect);
    const float tall = size * kRadarAspect;
    return RadarPanel{kPanelMarginPixels, height - kPanelMarginPixels - tall, size, tall};
}

RadarPoint radar_project(const RadarPanel& panel, float u, float v, int radius_tiles,
                         float height_metres) noexcept {
    const float scale = panel.width / (2.0F * static_cast<float>(std::max(radius_tiles, 1)));
    // The relief lifts a point UP the panel: a hill expresses itself as pixels above the plane.
    const float lift = (height_metres / kMetresPerTile) * std::cos(kRadarElevationRadians) * scale;
    return RadarPoint{(panel.width * 0.5F) + (u * scale),
                      (panel.height * 0.5F) - (v * std::sin(kRadarElevationRadians) * scale)
                          - lift};
}

namespace {

/// Clips a segment to the oval, in the panel's own unit-disc space: the oval IS the unit circle
/// once each axis is divided by its semi-axis, so the inside range of the segment is a quadratic's
/// two roots and both endpoints can be pulled exactly onto the ellipse. False when the segment
/// misses the oval entirely, in which case nothing is drawn -- a segment drawn across the panel's
/// corners would break the containment claim the whole radar is held to.
[[nodiscard]] bool clip_to_oval(const RadarPanel& panel, RadarPoint& a, RadarPoint& b) noexcept {
    const float half_x = std::max(panel.width * 0.5F, 1.0e-3F);
    const float half_y = std::max(panel.height * 0.5F, 1.0e-3F);
    const float x0 = (a.px - half_x) / half_x;
    const float y0 = (a.py - half_y) / half_y;
    const float x1 = (b.px - half_x) / half_x;
    const float y1 = (b.py - half_y) / half_y;
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float aa = (dx * dx) + (dy * dy);
    const float bb = 2.0F * ((x0 * dx) + (y0 * dy));
    const float cc = (x0 * x0) + (y0 * y0) - 1.0F;
    if (aa <= 0.0F) {
        return cc <= 0.0F; // a zero-length segment is a point: on the face or nowhere
    }
    const float discriminant = (bb * bb) - (4.0F * aa * cc);
    if (discriminant <= 0.0F) {
        return false; // the line touches the oval nowhere
    }
    const float root = std::sqrt(discriminant);
    const float lo = std::max(0.0F, (-bb - root) / (2.0F * aa));
    const float hi = std::min(1.0F, (-bb + root) / (2.0F * aa));
    if (hi <= lo) {
        return false;
    }
    const auto at = [&](float t) {
        return RadarPoint{half_x + ((x0 + (dx * t)) * half_x), half_y + ((y0 + (dy * t)) * half_y)};
    };
    a = at(lo);
    b = at(hi);
    return true;
}

} // namespace

std::vector<RadarSegment> radar_mesh(std::uint64_t seed, float viewer_x, float viewer_z,
                                     float yaw_radians, const RadarPanel& panel, int radius_tiles,
                                     int step_tiles, const TerrainCache* terrain) {
    std::vector<RadarSegment> segments;
    if (radius_tiles <= 0 || step_tiles <= 0 || panel.width <= 0.0F || panel.height <= 0.0F) {
        return segments;
    }
    const float sine = std::sin(yaw_radians);
    const float cosine = std::cos(yaw_radians);
    const int base_x = static_cast<int>(std::floor(viewer_x));
    const int base_z = static_cast<int>(std::floor(viewer_z));
    const int corners = ((2 * radius_tiles) / step_tiles) + 1;
    // A corner just past the radius still covers the rim, so the mesh reaches the edge of the range
    // rather than stopping a step short of it.
    const float reach = static_cast<float>(radius_tiles + step_tiles);
    const std::size_t count = static_cast<std::size_t>(corners) * static_cast<std::size_t>(corners);

    // The grid, projected once per corner: row-major, and a corner outside the radius is left
    // unsampled, so no height is ever read for terrain the radar cannot see.
    std::vector<RadarPoint> points(count);
    std::vector<std::uint8_t> sampled(count, 0);
    for (int row = 0; row < corners; ++row) {
        for (int column = 0; column < corners; ++column) {
            const int tile_x = base_x - radius_tiles + (column * step_tiles);
            const int tile_z = base_z - radius_tiles + (row * step_tiles);
            const float dx = static_cast<float>(tile_x) - viewer_x;
            const float dz = static_cast<float>(tile_z) - viewer_z;
            if (std::hypot(dx, dz) > reach) {
                continue;
            }
            // The same forward/right the marks are placed with, so the mesh cannot disagree with
            // them about which way the tank is pointing.
            const float forward = (dx * sine) + (dz * cosine);
            const float right = (dx * cosine) - (dz * sine);
            const std::size_t index
                = (static_cast<std::size_t>(row) * static_cast<std::size_t>(corners))
                  + static_cast<std::size_t>(column);
            points.at(index) = radar_project(panel, right, forward, radius_tiles,
                                             corner_height_metres(terrain, seed, tile_x, tile_z));
            sampled.at(index) = 1;
        }
    }

    const auto edge = [&](std::size_t from, std::size_t to) {
        if (sampled.at(from) == 0 || sampled.at(to) == 0) {
            return; // one end is outside the radius, so there is no line there
        }
        RadarPoint a = points.at(from);
        RadarPoint b = points.at(to);
        if (clip_to_oval(panel, a, b)) {
            segments.push_back(RadarSegment{a, b});
        }
    };
    // Rows and columns, in a fixed order, so the same seed and the same viewer give the same list.
    const auto index_at = [corners](int row, int column) {
        return (static_cast<std::size_t>(row) * static_cast<std::size_t>(corners))
               + static_cast<std::size_t>(column);
    };
    for (int row = 0; row < corners; ++row) {
        for (int column = 0; column < corners; ++column) {
            if (column + 1 < corners) {
                edge(index_at(row, column), index_at(row, column + 1));
            }
            if (row + 1 < corners) {
                edge(index_at(row, column), index_at(row + 1, column));
            }
        }
    }
    return segments;
}

namespace {

/// Pulls a point toward the middle of the oval until it is inside the oval whose semi-axes are the
/// panel's, each reduced by `inset`. The pull is along the ray from the middle, so a direction is
/// kept. A point already inside is left exactly where it is.
[[nodiscard]] RadarPoint pin_inside(const RadarPanel& panel, RadarPoint p, float inset) noexcept {
    const float a = std::max((panel.width * 0.5F) - inset, 1.0F);
    const float b = std::max((panel.height * 0.5F) - inset, 1.0F);
    const float x = (p.px - (panel.width * 0.5F)) / a;
    const float y = (p.py - (panel.height * 0.5F)) / b;
    const float reach = std::hypot(x, y);
    if (reach <= 1.0F) {
        return p;
    }
    return RadarPoint{(panel.width * 0.5F) + (x / reach * a),
                      (panel.height * 0.5F) + (y / reach * b)};
}

/// Pulls a square mark's centre toward the middle until all four corners of its box are on the
/// face. A fixed inset from a flat oval is not enough (the corner of a box at the end of the long
/// axis reaches past the offset curve), so this measures the corners and shrinks until they fit.
[[nodiscard]] RadarPoint fit_box(const RadarPanel& panel, RadarPoint p, float half) noexcept {
    const float middle_x = panel.width * 0.5F;
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
    const float middle_x = panel.width * 0.5F;
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
    if (!viewer_unit.valid() || radius_tiles <= 0 || panel.width <= 0.0F) {
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
    marks.push_back(RadarMark{middle.px, middle.py, panel.width * kViewerHalfFraction, 0.0F,
                              RadarShape::Triangle, RadarTone::You, viewer->id});

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
                = panel.width
                  * (unit.kind == sim::UnitKind::Factory ? kBaseHalfFraction : kDotHalfFraction);
            const RadarPoint at
                = fit_box(panel, radar_project(panel, right, forward, radius_tiles), half);
            marks.push_back(RadarMark{
                at.px, at.py, half, 0.0F, base ? RadarShape::Hollow : RadarShape::Square,
                unit.side == viewer->side ? RadarTone::Yours : RadarTone::Hostile, unit.id});
            continue;
        }
        // Your own base, out of range: a mark at the rim, pointing the way to turn. The arrow
        // points along the GROUND bearing (the way to turn), though it sits where that bearing
        // meets the oval.
        const float length = std::max(distance, 1.0e-3F);
        const float ground_x = right / length;
        const float ground_y = forward / length;
        const float half = panel.width * kArrowHalfFraction;
        const RadarPoint at
            = fit_box(panel,
                      radar_project(panel, ground_x * static_cast<float>(radius_tiles),
                                    ground_y * static_cast<float>(radius_tiles), radius_tiles),
                      half);
        marks.push_back(RadarMark{at.px, at.py, half, std::atan2(ground_x, ground_y),
                                  RadarShape::Arrow, RadarTone::Yours, unit.id});
    }
    return marks;
}

float radar_nav_half(const RadarPanel& panel) noexcept { return panel.width * kNavHalfFraction; }

std::vector<RadarNav> radar_nav_marks(std::span<const sim::Unit> units, sim::UnitId viewer_unit,
                                      std::span<const NavMarks::Mark> marks, float yaw_radians,
                                      const RadarPanel& panel, int radius_tiles) {
    std::vector<RadarNav> shown;
    if (!viewer_unit.valid() || radius_tiles <= 0 || panel.width <= 0.0F) {
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
    const float half = radar_nav_half(panel);
    // The plate stays wholly on the face: its centre never goes past the oval inset by this much.
    const float inset = (kNavRimFraction * panel.width) + (1.5F * half);
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
        shown.push_back(RadarNav{at.px, at.py, half, mark.slot, mark.highlighted, !in_range});
    }
    return shown;
}

} // namespace protekt::game
