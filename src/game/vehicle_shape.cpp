#include "protekt/game/vehicle_shape.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <utility>

#include "protekt/game/scene.hpp"

namespace protekt::game {

namespace {

using Profile = std::vector<std::array<float, 2>>;

/// Counter-clockwise, whatever order the corners were written in: `shape_covers` and the mesh's
/// bevel both rely on one winding.
Profile counter_clockwise(Profile corners) {
    float area = 0.0F;
    for (std::size_t i = 0, j = corners.size() - 1; i < corners.size(); j = i++) {
        area += (corners.at(j)[0] * corners.at(i)[1]) - (corners.at(i)[0] * corners.at(j)[1]);
    }
    if (area < 0.0F) {
        std::reverse(corners.begin(), corners.end());
    }
    return corners;
}

/// A regular polygon: a wheel, a hub, a pivot.
Profile ngon(float cx, float cy, float radius, int sides) {
    Profile corners;
    for (int i = 0; i < sides; ++i) {
        const float angle
            = 2.0F * std::numbers::pi_v<float> * static_cast<float>(i) / static_cast<float>(sides);
        corners.push_back({cx + (radius * std::cos(angle)), cy + (radius * std::sin(angle))});
    }
    return corners;
}

/// A rounded slab standing on `y0`: the track run. `y1 - y0` is its height and each end is a
/// half-circle of `segments` facets.
Profile stadium(float x0, float x1, float y0, float y1, int segments) {
    const float radius = (y1 - y0) * 0.5F;
    const float cy = y0 + radius;
    Profile corners;
    for (int i = 0; i <= segments; ++i) { // the front end, bottom round to top
        const float angle = std::numbers::pi_v<float>
                            * (-0.5F + (static_cast<float>(i) / static_cast<float>(segments)));
        corners.push_back(
            {x1 - radius + (radius * std::cos(angle)), cy + (radius * std::sin(angle))});
    }
    for (int i = 0; i <= segments; ++i) { // the rear end, top round to bottom
        const float angle = std::numbers::pi_v<float>
                            * (0.5F + (static_cast<float>(i) / static_cast<float>(segments)));
        corners.push_back(
            {x0 + radius + (radius * std::cos(angle)), cy + (radius * std::sin(angle))});
    }
    return corners;
}

/// Appends a part, its corners counter-clockwise whatever order they were written in.
void add_part(std::vector<ShapePart>& parts, Profile profile, float lo, float hi, bool mirrored,
              float bevel, ShapeRole role, bool lit = true, ShapeGroup group = ShapeGroup::Hull) {
    parts.push_back(ShapePart{counter_clockwise(std::move(profile)), lo, hi, mirrored, bevel, role,
                              lit, group});
}

/// THE SCAVENGER'S SHAPE. Every number the scavenger's look is made of is in this function. Read it
/// as the machine seen from its right-hand side, front at the right: two tracked pods with road
/// wheels, a floor between them, an open-topped HOPPER (two side walls and a tailgate) heaped with
/// scrap, a cab with a raked windscreen and wrapped glass, a stack, and a twin-armed BOOM that
/// rises from behind the cab, kinks at an elbow and reaches forward to a pincer that closes on the
/// scrap.
std::vector<ShapePart> author_scavenger() {
    std::vector<ShapePart> parts;
    const auto add = [&parts](Profile profile, float lo, float hi, bool mirrored, float bevel,
                              ShapeRole role, bool lit = true) {
        add_part(parts, std::move(profile), lo, hi, mirrored, bevel, role, lit);
    };

    // The track pods: 2.5 m by 0.45 m, 0.24 m wide each, their outer faces 0.60 m from the
    // centreline -- the tank's own half-width, so the machine fits where a tank does.
    constexpr float kPodLo = 0.36F;
    constexpr float kPodHi = 0.60F;
    constexpr float kPodTop = 0.45F;
    constexpr float kPodRear = -1.25F;
    constexpr float kPodFront = 1.25F;
    add(stadium(kPodRear, kPodFront, 0.0F, kPodTop, 6), kPodLo, kPodHi, true, 0.04F,
        ShapeRole::Track);

    // Five road wheels: a dark ring cut into the pod's face, a metal hub standing proud of it.
    constexpr int kWheels = 5;
    constexpr float kWheelFirst = -1.025F;
    constexpr float kWheelPitch = 0.5125F;
    constexpr float kRingRadius = 0.14F;
    constexpr float kHubRadius = 0.06F;
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add(ngon(cx, kPodTop * 0.5F, kRingRadius, 8), kPodLo - 0.01F, kPodHi + 0.01F, true, 0.0F,
            ShapeRole::Glass, false);
    }
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add(ngon(cx, kPodTop * 0.5F, kHubRadius, 6), kPodLo, kPodHi + 0.02F, true, 0.0F,
            ShapeRole::Metal);
    }

    // The floor the hopper and cab stand on, between the pods and level with their tops.
    constexpr float kFloorTop = 0.58F;
    add({{-1.2F, 0.40F}, {1.15F, 0.40F}, {1.15F, kFloorTop}, {-1.2F, kFloorTop}}, -0.38F, 0.38F,
        false, 0.03F, ShapeRole::Hull);

    // The hopper: two side walls 0.12 m thick, 0.65 m tall, and a tailgate across the back.
    constexpr float kHopperRear = -1.20F;
    constexpr float kHopperFront = 0.30F;
    constexpr float kWallTop = 1.10F;
    constexpr float kWallInner = 0.44F;
    constexpr float kWallOuter = 0.56F;
    add({{kHopperRear, kPodTop},
         {kHopperFront, kPodTop},
         {kHopperFront, kWallTop},
         {kHopperRear, kWallTop}},
        kWallInner, kWallOuter, true, 0.02F, ShapeRole::Hull);
    add({{-1.24F, kPodTop}, {-1.16F, kPodTop}, {-1.16F, kWallTop}, {-1.24F, kWallTop}}, -kWallOuter,
        kWallOuter, false, 0.02F, ShapeRole::Hull);
    // The scrap heaped in it, standing a quarter-metre over the walls' rim.
    add({{-1.10F, kFloorTop},
         {0.20F, kFloorTop},
         {0.20F, 1.10F},
         {-0.20F, 1.34F},
         {-0.90F, 1.28F},
         {-1.10F, 1.00F}},
        -kWallInner, kWallInner, false, 0.05F, ShapeRole::Load);

    // The cab: a cabover standing on the pods' tops, its windscreen raked back, and its glass cut
    // through both flanks as one wrapped band, as the recycler's vision band is.
    add({{0.30F, kPodTop}, {1.20F, kPodTop}, {1.20F, 0.95F}, {0.95F, 1.25F}, {0.30F, 1.25F}},
        -0.44F, 0.44F, false, 0.05F, ShapeRole::Hull);
    add({{0.55F, 0.85F}, {1.12F, 0.85F}, {0.93F, 1.15F}, {0.55F, 1.15F}}, -0.45F, 0.45F, false,
        0.0F, ShapeRole::Glass, false);
    // The exhaust stack on the cab's right shoulder.
    add({{0.34F, 1.20F}, {0.44F, 1.20F}, {0.42F, 1.55F}, {0.36F, 1.55F}}, 0.30F, 0.40F, false,
        0.02F, ShapeRole::Metal);

    // The boom: two arms, one either side of the cab, 0.46 to 0.56 m out. The upper arm climbs
    // from behind the cab to an elbow 1.7 m up; the forearm drops from it to the wrist over the
    // cab's nose; a yoke across the wrist carries the pincer's two jaws.
    constexpr float kArmLo = 0.46F;
    constexpr float kArmHi = 0.56F;
    add({{0.357F, 1.06F}, {0.907F, 1.66F}, {0.793F, 1.74F}, {0.243F, 1.14F}}, kArmLo, kArmHi, true,
        0.02F, ShapeRole::Metal);
    add({{0.815F, 1.651F}, {1.515F, 1.151F}, {1.585F, 1.249F}, {0.885F, 1.749F}}, kArmLo, kArmHi,
        true, 0.02F, ShapeRole::Metal);
    add(ngon(0.85F, 1.70F, 0.09F, 10), kArmLo - 0.02F, kArmHi + 0.03F, true, 0.0F,
        ShapeRole::Metal); // the elbow pin
    add({{1.44F, 1.10F}, {1.60F, 1.10F}, {1.60F, 1.30F}, {1.44F, 1.30F}}, -0.50F, 0.50F, false,
        0.02F, ShapeRole::Metal);
    add({{1.48F, 1.28F}, {1.82F, 1.12F}, {1.84F, 1.02F}, {1.50F, 1.15F}}, -0.34F, 0.34F, false,
        0.02F, ShapeRole::Metal); // the upper jaw
    add({{1.46F, 1.12F}, {1.50F, 0.80F}, {1.88F, 0.62F}, {1.66F, 0.96F}}, -0.34F, 0.34F, false,
        0.02F, ShapeRole::Metal); // the lower jaw
    return parts;
}

/// THE TANK'S SHAPE. The machine seen from its right-hand side, front at the right: two track runs
/// with five road wheels, a low hull whose front leans back in a glacis to the compartment roof and
/// steps down aft to the engine deck with its dark grille, a fender over each track with a dark
/// line under it, and the TURRET group -- a housing with a raked roof front, a rear bustle, a
/// commander's hatch on the right of the roof and the vision slit cut in it. The BARREL is not
/// here: it elevates, and the weapon code owns it. The sizes are the tank's of old (2.6 m of
/// track, 1.2 m across the fenders, the turret on `kTankTurretBase`), so this changes how it is
/// described and not how big it is.
std::vector<ShapePart> author_tank() {
    std::vector<ShapePart> parts;
    const auto add
        = [&parts](Profile profile, float lo, float hi, bool mirrored, float bevel, ShapeRole role,
                   bool lit = true, ShapeGroup group = ShapeGroup::Hull) {
              add_part(parts, std::move(profile), lo, hi, mirrored, bevel, role, lit, group);
          };

    // The fenders' outer edge, 0.60 m, is the tank's half-width and the widest thing on it.
    constexpr float kHalfWidth = kHullHalfMetres;
    constexpr float kTrackLo = kHalfWidth * 0.7F;
    constexpr float kTrackHi = kHalfWidth * 0.93F;
    constexpr float kTrackHeight = 0.4F;
    constexpr float kTrackEnd = 1.3F;

    // The hull between the tracks, painted FIRST so the wheels' cut rings clear it on the plate:
    // the fighting compartment, whose front leans back from the foot of the glacis to the roof, and
    // the lower engine deck aft of it, tucked 0.05 m under it.
    constexpr float kHullHalf = kTrackLo;
    constexpr float kCompartmentTop = 0.85F;
    constexpr float kEngineDeckTop = 0.62F;
    add({{-0.45F, 0.0F}, {1.2F, 0.0F}, {0.55F, kCompartmentTop}, {-0.45F, kCompartmentTop}},
        -kHullHalf, kHullHalf, false, 0.03F, ShapeRole::Hull);
    add({{-1.2F, 0.0F}, {-0.4F, 0.0F}, {-0.4F, kEngineDeckTop}, {-1.2F, kEngineDeckTop}},
        -kHullHalf, kHullHalf, false, 0.03F, ShapeRole::Hull);
    // The grille on the deck, dark like the tracks and standing 3 cm proud of it.
    add({{-1.1F, kEngineDeckTop},
         {-0.55F, kEngineDeckTop},
         {-0.55F, kEngineDeckTop + 0.03F},
         {-1.1F, kEngineDeckTop + 0.03F}},
        -0.28F, 0.28F, false, 0.0F, ShapeRole::Track);

    // The track runs: 2.6 m by 0.4 m, 0.14 m wide each, 0.42 m out from the centreline.
    add(stadium(-kTrackEnd, kTrackEnd, 0.0F, kTrackHeight, 4), kTrackLo, kTrackHi, true, 0.02F,
        ShapeRole::Track);

    // Five road wheels: a dark ring cut into the run's face, a metal hub standing proud of it.
    constexpr int kWheels = 5;
    constexpr float kWheelFirst = -1.0F;
    constexpr float kWheelPitch = 0.5F;
    constexpr float kRingRadius = 0.13F;
    constexpr float kHubRadius = 0.05F;
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add(ngon(cx, kTrackHeight * 0.5F, kRingRadius, 8), kTrackLo - 0.01F, kTrackHi + 0.01F, true,
            0.0F, ShapeRole::Glass, false);
    }
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add(ngon(cx, kTrackHeight * 0.5F, kHubRadius, 6), kTrackLo, kTrackHi + 0.02F, true, 0.0F,
            ShapeRole::Metal);
    }

    // The fenders over the tracks, a shelf wider than the run under it, and the dark line where
    // each meets the run, which is a cut on the plate and a shadow line on the mesh.
    constexpr float kFenderLo = kHalfWidth * 0.65F;
    constexpr float kFenderHi = kHalfWidth * 0.98F;
    constexpr float kFenderBottom = 0.38F;
    constexpr float kFenderTop = 0.44F;
    add({{-1.2F, kFenderBottom}, {1.2F, kFenderBottom}, {1.2F, kFenderTop}, {-1.2F, kFenderTop}},
        kFenderLo, kFenderHi, true, 0.015F, ShapeRole::Hull);
    add({{-1.15F, kFenderBottom - 0.025F},
         {1.15F, kFenderBottom - 0.025F},
         {1.15F, kFenderBottom + 0.015F},
         {-1.15F, kFenderBottom + 0.015F}},
        kFenderLo - 0.01F, kHalfWidth, true, 0.0F, ShapeRole::Glass, false);

    // THE TURRET GROUP, from here on. The housing is 1.0 m long and 0.72 m across, its roof front
    // raked, its front face the barrel's root; the bustle is the rear overhang.
    constexpr float kTurretBack = -0.65F;
    constexpr float kTurretHalf = 0.36F;
    constexpr float kRoof = kTankTurretRoof;
    add({{kTurretBack, kTankTurretBase},
         {kTankTurretFront, kTankTurretBase},
         {kTankTurretFront, kRoof - 0.08F},
         {kTankTurretFront - 0.15F, kRoof},
         {kTurretBack, kRoof}},
        -kTurretHalf, kTurretHalf, false, 0.03F, ShapeRole::Hull, true, ShapeGroup::Turret);
    add({{-0.9F, kTankTurretBase + 0.05F},
         {kTurretBack, kTankTurretBase + 0.05F},
         {kTurretBack, kTankTurretBase + 0.3F},
         {-0.9F, kTankTurretBase + 0.3F}},
        -0.25F, 0.25F, false, 0.03F, ShapeRole::Hull, true, ShapeGroup::Turret);
    // The commander's hatch, a low trapezoid on the right of the roof, with its vision slit cut
    // through it.
    constexpr float kHatchLo = 0.12F;
    constexpr float kHatchHi = 0.34F;
    add({{-0.4F, kRoof}, {0.0F, kRoof}, {-0.04F, kRoof + 0.1F}, {-0.36F, kRoof + 0.1F}}, kHatchLo,
        kHatchHi, false, 0.02F, ShapeRole::Metal, true, ShapeGroup::Turret);
    add({{-0.34F, kRoof + 0.03F},
         {-0.06F, kRoof + 0.03F},
         {-0.06F, kRoof + 0.08F},
         {-0.34F, kRoof + 0.08F}},
        kHatchLo - 0.01F, kHatchHi + 0.01F, false, 0.0F, ShapeRole::Glass, false,
        ShapeGroup::Turret);
    return parts;
}

bool inside(const Profile& convex, float along, float up) {
    for (std::size_t i = 0, j = convex.size() - 1; i < convex.size(); j = i++) {
        const auto& a = convex.at(j);
        const auto& b = convex.at(i);
        if (((b[0] - a[0]) * (up - a[1])) - ((b[1] - a[1]) * (along - a[0])) < 0.0F) {
            return false;
        }
    }
    return true;
}

} // namespace

const std::vector<ShapePart>& shape_of(sim::UnitKind kind) {
    static const std::vector<ShapePart> scavenger = author_scavenger();
    static const std::vector<ShapePart> tank = author_tank();
    static const std::vector<ShapePart> none; // a kind with no authored shape has an empty one
    switch (kind) {
    case sim::UnitKind::Scavenger:
        return scavenger;
    case sim::UnitKind::Tank:
        return tank;
    default:
        return none;
    }
}

bool shape_covers(sim::UnitKind kind, float along, float up) {
    bool lit = false;
    for (const ShapePart& part : shape_of(kind)) {
        if (inside(part.profile, along, up)) {
            lit = part.lit;
        }
    }
    return lit;
}

ShapeExtent shape_extent(sim::UnitKind kind) {
    const std::vector<ShapePart>& parts = shape_of(kind);
    if (parts.empty()) {
        return ShapeExtent{0.0F, 0.0F, 0.0F, 0.0F};
    }
    ShapeExtent extent{1.0e9F, -1.0e9F, 0.0F, 0.0F};
    for (const ShapePart& part : parts) {
        for (const auto& corner : part.profile) {
            if (part.lit) {
                extent.along_min = std::min(extent.along_min, corner[0]);
                extent.along_max = std::max(extent.along_max, corner[0]);
                extent.up_max = std::max(extent.up_max, corner[1]);
            }
        }
        extent.across_max
            = std::max({extent.across_max, std::abs(part.across_lo), std::abs(part.across_hi)});
    }
    return extent;
}

} // namespace protekt::game
