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

/// How far a hull's box corner is cut, in metres along each edge of the corner. AT shipped 0.10 m;
/// the owner asked for more and AY raised it to 0.25 m. Round 15 then brought it DOWN: the owner
/// said "the turrets look like snowmen" (the turret's base and pedestal are both cut, and at 0.25
/// they read as two stacked balls) and "let's tweak down the rounding on everything", and chose a
/// baked value over a run-time knob ("I don't want to try with different switches"). 0.16 is the
/// assistant's proposal between AT's 0.10 and AY's 0.25: a LOOK the owner accepts or rejects by
/// seeing it, not a measured optimum. A smaller part takes the share of its own edge instead
/// (`kMaxCutShare`). Every lit hull or load part of every authored kind has its corners cut by
/// this, so the hull has no 90 degree edge for smooth shading to leave sharp.
constexpr float kHullChamfer = 0.16F;

/// The most of an edge one cut may take (AY; AT held 0.4). Two cuts share an edge, so each at most
/// this share leaves `1 - 2 * 0.45` = a tenth of it flat; at 0.5 they would meet at its middle and
/// leave a zero-length edge for the mesh's miter, and past 0.5 they would cross.
constexpr float kMaxCutShare = 0.45F;

/// The corners of `profile` that turn more than `max_uncut_turn` degrees -- the ones smooth shading
/// (crease 75 degrees) would leave as hard edges -- each cut into two, `cut` metres in from the
/// corner along both its edges (never more than `kMaxCutShare` of the shorter edge, so neighbouring
/// cuts do not meet). The two cuts are the same distance out, so a corner that turned t degrees
/// becomes two that turn t/2: a 90 degree box corner becomes two 45s, the band smooth shading
/// rounds. A profile that was convex stays convex, and a corner already gentle is kept as it was.
Profile chamfer_corners(const Profile& profile, float cut, float max_uncut_turn) {
    constexpr float kDegrees = 180.0F / std::numbers::pi_v<float>;
    const std::size_t n = profile.size();
    Profile cut_profile;
    cut_profile.reserve(2 * n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto& before = profile.at((i + n - 1) % n);
        const auto& here = profile.at(i);
        const auto& after = profile.at((i + 1) % n);
        const float in_x = here[0] - before[0];
        const float in_y = here[1] - before[1];
        const float out_x = after[0] - here[0];
        const float out_y = after[1] - here[1];
        const float in_length = std::hypot(in_x, in_y);
        const float out_length = std::hypot(out_x, out_y);
        const float turn
            = std::abs(std::atan2((in_x * out_y) - (in_y * out_x), (in_x * out_x) + (in_y * out_y)))
              * kDegrees;
        if (turn <= max_uncut_turn) {
            cut_profile.push_back(here);
            continue;
        }
        const float d = std::min({cut, kMaxCutShare * in_length, kMaxCutShare * out_length});
        cut_profile.push_back({here[0] - (in_x / in_length * d), here[1] - (in_y / in_length * d)});
        cut_profile.push_back(
            {here[0] + (out_x / out_length * d), here[1] + (out_y / out_length * d)});
    }
    return cut_profile;
}

/// Appends a part, its corners counter-clockwise whatever order they were written in. A lit HULL or
/// LOAD part -- the body of the machine -- has its corners cut (`rounded_profile`); a cut, and a
/// metal, glass or track part, keeps the corners it was written with.
void add_part(std::vector<ShapePart>& parts, Profile profile, float lo, float hi, bool mirrored,
              float bevel, ShapeRole role, bool lit = true, ShapeGroup group = ShapeGroup::Hull) {
    const bool body = lit && (role == ShapeRole::Hull || role == ShapeRole::Load);
    Profile corners
        = body ? rounded_profile(std::move(profile)) : counter_clockwise(std::move(profile));
    parts.push_back(ShapePart{std::move(corners), lo, hi, mirrored, bevel, role, lit, group});
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

/// THE HOVER TANK'S SHAPE. The machine seen from its right-hand side, front at the right: a low
/// WEDGE that is deep at the tail and runs down to the nose, no track and no wheel anywhere, so it
/// cannot be taken for a tank. A part is a side profile swept across, so the wedge's narrowing in
/// plan is written as three STEPS, each as wide as the section it stands for (1.2 m at the tail,
/// 0.8 m, then 0.4 m at the nose), the deck sloping through all three as one line. A swept-back
/// dorsal fin stands at the tail -- the model's highest point -- and a small raked turret sits on
/// the deck ahead of the wedge's tallest point. The SKIRT under the hull and the BARREL are not
/// here: the skirt is raw unlit colour and the barrel elevates, so the mesh and the plate add them
/// from the numbers in scene.hpp (`kHoverSkirtBottom`, `kHoverBarrelTip`, ...).
std::vector<ShapePart> author_hover_tank() {
    std::vector<ShapePart> parts;

    // The deck, 0.85 m up at the tail and 0.5 m at the nose, a straight line between.
    constexpr float kTailTop = 0.85F;
    constexpr float kNoseTop = 0.5F;
    const auto deck = [](float along) {
        return kTailTop
               + ((kNoseTop - kTailTop) * (along - kHoverTail) / (kHoverNose - kHoverTail));
    };
    struct Step {
        float rear;
        float front;
        float half; ///< half the width across the step
    };
    constexpr std::array<Step, 3> kSteps{
        {{kHoverTail, -0.4F, kHullHalfMetres}, {-0.4F, 0.55F, 0.4F}, {0.55F, kHoverNose, 0.2F}}};
    for (const Step& step : kSteps) {
        add_part(parts,
                 {{step.rear, kHoverHullBottom},
                  {step.front, kHoverHullBottom},
                  {step.front, deck(step.front)},
                  {step.rear, deck(step.rear)}},
                 -step.half, step.half, false, 0.03F, ShapeRole::Hull);
    }

    // The fin, 8 cm thick, swept back, rooted 10 cm into the deck: 1.1 m, the model's top.
    add_part(parts, {{-1.25F, 0.75F}, {-0.75F, 0.75F}, {-0.95F, 1.1F}, {-1.2F, 1.1F}}, -0.04F,
             0.04F, false, 0.01F, ShapeRole::Metal);

    // The turret group: 0.64 m long and 0.52 m across, its roof front raked, its base sunk into the
    // sloping deck and its front face the barrel's root.
    constexpr float kTurretTop = kHoverTurretBase + 0.3F;
    add_part(parts,
             {{-0.22F, kHoverTurretBase},
              {kHoverTurretFront, kHoverTurretBase},
              {kHoverTurretFront, kTurretTop - 0.08F},
              {kHoverTurretFront - 0.1F, kTurretTop},
              {-0.22F, kTurretTop}},
             -0.26F, 0.26F, false, 0.03F, ShapeRole::Hull, true, ShapeGroup::Turret);
    return parts;
}

/// Where the scout's nose is authored to, before its tip is cut. The tests pin the nose at 1.3 m,
/// and cutting a tip takes its point off, so the corner is written 0.123 m further out and the
/// rounded nose ends at 1.3 (AY). The same holds for the bomber's tail and the turret's base below:
/// each outermost corner is pushed out by what its own cut takes, so the machine does not shrink.
/// Round 15 (cut 0.25 -> 0.16): this nose tip's cut was never the full cut -- its short edge caps
/// it at 0.45 of 0.317 m, 0.142 -- so it is unchanged; the bomber's and turret's tips were
/// re-derived.
constexpr float kScoutNoseTip = 1.423F;

/// The body the scout and the missile scout share: a low wedge on TWO SMALL WHEELS with a clear
/// canopy cut through its flanks. The body is 0.8 m across and the wheels stand outside it, their
/// outer hubs 0.60 m from the centreline -- the tank's own half-width, so the machine fits where a
/// tank does. A wheel is a dark tyre, a clear ring cut into its face and a metal hub standing
/// proud, as the tank's road wheels are. The scout adds a mast and the missile scout a rack to it.
void add_scout_chassis(std::vector<ShapePart>& parts) {
    // The wedge, rear to nose: a blunt tail, the roof's crest 0.86 m up, a nose that runs down.
    add_part(parts,
             {{-1.25F, 0.22F},
              {1.15F, 0.22F},
              {kScoutNoseTip, 0.38F},
              {0.9F, 0.66F},
              {-0.2F, 0.86F},
              {-1.25F, 0.66F}},
             -0.4F, 0.4F, false, 0.04F, ShapeRole::Hull);

    constexpr float kWheelRadius = 0.3F;
    constexpr float kWheelAhead = 0.85F;
    constexpr float kRingRadius = 0.17F;
    constexpr float kHubRadius = 0.07F;
    for (const float along : {-kWheelAhead, kWheelAhead}) {
        add_part(parts, ngon(along, kWheelRadius, kWheelRadius, 12), 0.38F, 0.58F, true, 0.02F,
                 ShapeRole::Track);
        add_part(parts, ngon(along, kWheelRadius, kRingRadius, 8), 0.37F, 0.59F, true, 0.0F,
                 ShapeRole::Glass, false);
        add_part(parts, ngon(along, kWheelRadius, kHubRadius, 6), 0.38F, 0.6F, true, 0.0F,
                 ShapeRole::Metal);
    }

    // The canopy, a window wrapped through both flanks 1 cm proud of the body's side, with a roof
    // at least 9 cm thick over it.
    add_part(parts, {{0.0F, 0.52F}, {0.6F, 0.52F}, {0.56F, 0.6F}, {0.25F, 0.68F}, {0.0F, 0.68F}},
             -0.41F, 0.41F, false, 0.0F, ShapeRole::Glass, false);
}

/// THE SCOUT'S SHAPE. The chassis, and a thing that looks and points no gun: a slim mast rising
/// from the rear deck on the centreline with a tilted radar dish on its head, 1.24 m up, the
/// machine's top.
std::vector<ShapePart> author_scout() {
    std::vector<ShapePart> parts;
    add_scout_chassis(parts);
    add_part(parts, {{-0.64F, 0.6F}, {-0.56F, 0.6F}, {-0.56F, 1.06F}, {-0.64F, 1.06F}}, -0.05F,
             0.05F, false, 0.01F, ShapeRole::Metal);
    add_part(parts, {{-0.7F, 1.06F}, {-0.58F, 1.04F}, {-0.44F, 1.28F}, {-0.56F, 1.3F}}, -0.2F, 0.2F,
             false, 0.02F, ShapeRole::Metal);
    return parts;
}

/// THE MISSILE SCOUT'S SHAPE. The scout's chassis with, in place of the mast, a RACK of missile
/// tubes on the rear deck, slanted up and forward: a block 0.56 m across with two dark slits cut
/// down its length that make it three tubes, and a nose cone on each. The rack is the TURRET group:
/// it turns to the bearing of the target. 1.24 m up at the noses.
std::vector<ShapePart> author_msl_scout() {
    std::vector<ShapePart> parts;
    add_scout_chassis(parts);
    constexpr float kRackLo = -0.28F;
    constexpr float kRackHi = 0.28F;
    add_part(parts, {{-1.1F, 0.66F}, {-0.4F, 0.66F}, {0.0F, 1.1F}, {-0.7F, 1.1F}}, kRackLo, kRackHi,
             false, 0.03F, ShapeRole::Metal, true, ShapeGroup::Turret);
    for (const float slit : {0.233F, 0.467F}) { // metres from the rack's rear edge, along the base
        const auto at = [&](float offset, float rise) {
            return std::array<float, 2>{-1.1F + (0.4F * rise) + slit + offset,
                                        0.66F + (0.44F * rise)};
        };
        add_part(parts,
                 {at(-0.015F, 0.05F), at(0.015F, 0.05F), at(0.015F, 0.95F), at(-0.015F, 0.95F)},
                 kRackLo - 0.01F, kRackHi + 0.01F, false, 0.0F, ShapeRole::Glass, false,
                 ShapeGroup::Turret);
    }
    for (const float tube : {0.1167F, 0.35F, 0.5833F}) { // each tube's nose cone
        const float root = -0.7F + tube;
        add_part(parts, {{root - 0.07F, 1.1F}, {root + 0.07F, 1.1F}, {root + 0.1F, 1.24F}}, -0.2F,
                 0.2F, false, 0.02F, ShapeRole::Metal, true, ShapeGroup::Turret);
    }
    return parts;
}

/// The CHASSIS the rocket tank and the assault tank share, the tank's own: two track runs with five
/// road wheels, the low hull whose front leans back in a glacis to the compartment roof and steps
/// down aft to the engine deck with its dark grille, and a fender over each track with a dark line
/// under it. 2.6 m of track, 1.2 m across the fenders. Each kind adds its own turret group on
/// top, from `kTankTurretBase` (0.8 m) up.
void add_tank_chassis(std::vector<ShapePart>& parts) {
    constexpr float kHalfWidth = kHullHalfMetres;
    constexpr float kTrackLo = kHalfWidth * 0.7F;
    constexpr float kTrackHi = kHalfWidth * 0.93F;
    constexpr float kTrackHeight = 0.4F;
    constexpr float kTrackEnd = 1.3F;

    // The hull between the tracks, painted FIRST so the wheels' cut rings clear it on the plate.
    constexpr float kHullHalf = kTrackLo;
    constexpr float kCompartmentTop = 0.85F;
    constexpr float kEngineDeckTop = 0.62F;
    add_part(parts,
             {{-0.45F, 0.0F}, {1.2F, 0.0F}, {0.55F, kCompartmentTop}, {-0.45F, kCompartmentTop}},
             -kHullHalf, kHullHalf, false, 0.03F, ShapeRole::Hull);
    add_part(parts,
             {{-1.2F, 0.0F}, {-0.4F, 0.0F}, {-0.4F, kEngineDeckTop}, {-1.2F, kEngineDeckTop}},
             -kHullHalf, kHullHalf, false, 0.03F, ShapeRole::Hull);
    // The grille on the deck, dark like the tracks and standing 3 cm proud of it.
    add_part(parts,
             {{-1.1F, kEngineDeckTop},
              {-0.55F, kEngineDeckTop},
              {-0.55F, kEngineDeckTop + 0.03F},
              {-1.1F, kEngineDeckTop + 0.03F}},
             -0.28F, 0.28F, false, 0.0F, ShapeRole::Track);

    add_part(parts, stadium(-kTrackEnd, kTrackEnd, 0.0F, kTrackHeight, 4), kTrackLo, kTrackHi, true,
             0.02F, ShapeRole::Track);

    // Five road wheels: a dark ring cut into the run's face, a metal hub standing proud of it.
    constexpr int kWheels = 5;
    constexpr float kWheelFirst = -1.0F;
    constexpr float kWheelPitch = 0.5F;
    constexpr float kRingRadius = 0.13F;
    constexpr float kHubRadius = 0.05F;
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add_part(parts, ngon(cx, kTrackHeight * 0.5F, kRingRadius, 8), kTrackLo - 0.01F,
                 kTrackHi + 0.01F, true, 0.0F, ShapeRole::Glass, false);
    }
    for (int i = 0; i < kWheels; ++i) {
        const float cx = kWheelFirst + (kWheelPitch * static_cast<float>(i));
        add_part(parts, ngon(cx, kTrackHeight * 0.5F, kHubRadius, 6), kTrackLo, kTrackHi + 0.02F,
                 true, 0.0F, ShapeRole::Metal);
    }

    // The fenders over the tracks, a shelf wider than the run under it, and the dark line where
    // each meets the run.
    constexpr float kFenderLo = kHalfWidth * 0.65F;
    constexpr float kFenderHi = kHalfWidth * 0.98F;
    constexpr float kFenderBottom = 0.38F;
    constexpr float kFenderTop = 0.44F;
    add_part(
        parts,
        {{-1.2F, kFenderBottom}, {1.2F, kFenderBottom}, {1.2F, kFenderTop}, {-1.2F, kFenderTop}},
        kFenderLo, kFenderHi, true, 0.015F, ShapeRole::Hull);
    add_part(parts,
             {{-1.15F, kFenderBottom - 0.025F},
              {1.15F, kFenderBottom - 0.025F},
              {1.15F, kFenderBottom + 0.015F},
              {-1.15F, kFenderBottom + 0.015F}},
             kFenderLo - 0.01F, kHalfWidth, true, 0.0F, ShapeRole::Glass, false);
}

/// THE ROCKET TANK'S SHAPE. The tank's chassis and, in the TURRET group where a gun would be, a
/// low mount and a broad LAUNCHER RACK slanted up and forward, with a dark lane cut down its
/// flank. No gun and no barrel anywhere. The rack is 0.84 m across and 1.3 m long on the slant,
/// and its top, with the team-colour panel the builder lays on it, stays under the 1.2 m the
/// health bar is anchored at.
std::vector<ShapePart> author_rocket_tank() {
    std::vector<ShapePart> parts;
    add_tank_chassis(parts);
    // The mount, 0.5 m long and 0.4 m across, sunk into the compartment roof.
    add_part(parts,
             {{-0.4F, kTankTurretBase}, {0.1F, kTankTurretBase}, {0.1F, 0.98F}, {-0.4F, 0.98F}},
             -0.2F, 0.2F, false, 0.02F, ShapeRole::Hull, true, ShapeGroup::Turret);
    // The rack: a parallelogram, its front 0.45 m forward of its rear over a 0.2 m rise.
    add_part(parts,
             {{-0.75F, 0.95F}, {0.1F, 0.95F}, {0.55F, kRocketRackTop}, {-0.3F, kRocketRackTop}},
             -0.42F, 0.42F, false, 0.03F, ShapeRole::Metal, true, ShapeGroup::Turret);
    add_part(parts, {{-0.4F, 1.0F}, {0.0F, 1.0F}, {0.225F, 1.1F}, {-0.175F, 1.1F}}, -0.43F, 0.43F,
             false, 0.0F, ShapeRole::Glass, false, ShapeGroup::Turret);
    return parts;
}

/// THE ASSAULT TANK'S SHAPE. The tank's chassis under a TALLER, WIDER turret block than the tank's:
/// 1.2 m long, 1.0 m across and 0.7 m tall, with a plain flat roof and no bustle, and a commander's
/// hatch low on the right of the roof. The BARREL is not here -- it elevates, and the weapon code
/// owns it -- and neither is the team-colour panel on the roof; the mesh and the plate add them
/// from the numbers in scene.hpp (`kAssaultTurretFront`, `kAssaultBarrelTip`, ...).
std::vector<ShapePart> author_assault_tank() {
    std::vector<ShapePart> parts;
    add_tank_chassis(parts);
    add_part(parts,
             {{-0.7F, kTankTurretBase},
              {kAssaultTurretFront, kTankTurretBase},
              {kAssaultTurretFront, kAssaultTurretRoof},
              {-0.7F, kAssaultTurretRoof}},
             -0.5F, 0.5F, false, 0.04F, ShapeRole::Hull, true, ShapeGroup::Turret);
    add_part(parts,
             {{-0.55F, kAssaultTurretRoof},
              {-0.25F, kAssaultTurretRoof},
              {-0.28F, kAssaultTurretRoof + 0.08F},
              {-0.52F, kAssaultTurretRoof + 0.08F}},
             0.32F, 0.44F, false, 0.02F, ShapeRole::Metal, true, ShapeGroup::Turret);
    return parts;
}

/// THE MORTAR BIKE'S SHAPE. The machine seen from its right-hand side, front at the right: a BIKE,
/// two big wheels in line on the centreline (a dark tyre, a clear ring cut into its face and a
/// metal hub standing proud, as the scout's wheels are), a slim frame between them, a seat, a fork
/// raked up and back to the handlebars, and, in the TURRET group, the mortar: a mount on the frame
/// and a SHORT STUBBY TUBE slanted up and forward at 45 degrees -- it lobs -- with a thicker collar
/// at its mouth and the mouth's bore cut in it. Nothing stands outside the handlebars' 0.7 m, and
/// the mortar's top stays under the 1.2 m the health bar is anchored at.
std::vector<ShapePart> author_mortar_bike() {
    std::vector<ShapePart> parts;

    // The frame, 18 cm wide, a little narrower than the tyres so they show either side of it.
    add_part(parts, {{-0.7F, 0.38F}, {0.65F, 0.38F}, {0.7F, 0.55F}, {0.2F, 0.62F}, {-0.55F, 0.62F}},
             -0.09F, 0.09F, false, 0.03F, ShapeRole::Hull);
    // The seat, dark, on the rear of the frame.
    add_part(parts, {{-0.6F, 0.6F}, {-0.1F, 0.6F}, {-0.15F, 0.72F}, {-0.55F, 0.72F}}, -0.1F, 0.1F,
             false, 0.02F, ShapeRole::Track);

    constexpr float kWheelRadius = 0.33F;
    constexpr float kWheelAhead = 0.65F;
    constexpr float kRingRadius = 0.2F;
    constexpr float kHubRadius = 0.08F;
    for (const float along : {-kWheelAhead, kWheelAhead}) {
        add_part(parts, ngon(along, kWheelRadius, kWheelRadius, 12), -0.1F, 0.1F, false, 0.02F,
                 ShapeRole::Track);
        add_part(parts, ngon(along, kWheelRadius, kRingRadius, 8), -0.11F, 0.11F, false, 0.0F,
                 ShapeRole::Glass, false);
        add_part(parts, ngon(along, kWheelRadius, kHubRadius, 6), -0.12F, 0.12F, false, 0.0F,
                 ShapeRole::Metal);
    }

    // The fork, from the front hub up and back to the bars, and the handlebars across its head.
    add_part(parts, {{0.62F, 0.33F}, {0.7F, 0.33F}, {0.5F, 0.85F}, {0.42F, 0.85F}}, -0.05F, 0.05F,
             false, 0.01F, ShapeRole::Metal);
    add_part(parts, {{0.38F, 0.85F}, {0.52F, 0.85F}, {0.52F, 0.92F}, {0.38F, 0.92F}}, -0.35F, 0.35F,
             false, 0.02F, ShapeRole::Metal);

    // THE TURRET GROUP. The mount, 0.5 m long and 0.44 m across, sunk into the frame; the tube,
    // 0.2 m across and 0.5 m long, and the collar, 0.3 m across and 0.2 m long, both on the line
    // from the breech (-0.2, 0.5) at 45 degrees.
    constexpr float kElevation = 0.785398F;
    constexpr float kBreechAlong = -0.2F;
    constexpr float kBreechUp = 0.5F;
    constexpr float kTubeLength = 0.5F;
    constexpr float kCollarLength = 0.2F;
    constexpr float kTubeHalf = 0.1F;
    constexpr float kCollarHalf = 0.15F;
    const auto along_axis = [&](float from, float to, float half) {
        const float dx = std::cos(kElevation);
        const float dy = std::sin(kElevation);
        const auto at = [&](float distance, float side) {
            return std::array<float, 2>{kBreechAlong + (dx * distance) - (dy * side),
                                        kBreechUp + (dy * distance) + (dx * side)};
        };
        return Profile{at(from, -half), at(to, -half), at(to, half), at(from, half)};
    };
    add_part(parts, {{-0.45F, 0.45F}, {0.05F, 0.45F}, {0.05F, 0.6F}, {-0.45F, 0.6F}}, -0.22F, 0.22F,
             false, 0.03F, ShapeRole::Hull, true, ShapeGroup::Turret);
    add_part(parts, along_axis(0.0F, kTubeLength, kTubeHalf), -kTubeHalf, kTubeHalf, false, 0.02F,
             ShapeRole::Track, true, ShapeGroup::Turret);
    add_part(parts, along_axis(kTubeLength, kTubeLength + kCollarLength, kCollarHalf), -kCollarHalf,
             kCollarHalf, false, 0.02F, ShapeRole::Track, true, ShapeGroup::Turret);
    // The bore: a dark cut in the collar's mouth, standing 1 cm proud of its face.
    add_part(
        parts,
        along_axis(kTubeLength + kCollarLength - 0.05F, kTubeLength + kCollarLength + 0.01F, 0.08F),
        -0.08F, 0.08F, false, 0.0F, ShapeRole::Glass, false, ShapeGroup::Turret);
    return parts;
}

/// THE WALKER'S SHAPE, BODY ONLY. The machine seen from its right-hand side, front at the right.
/// Its two legs -- the left the rear one and the right the front one, each a thigh and a shin
/// ending in a broad flat foot -- are `walker_legs`, below, because they walk. Over the hips (1.7 m
/// up) a body 1.0 m long with sloped shoulders and a cockpit glass wrapped through both flanks, and
/// a thin team-colour panel on its roof. The GUN ARM is not here: it elevates, so the mesh and the
/// plate add it from the numbers in scene.hpp (`kWalkerGunFront`, `kWalkerGunTip`, ...). Legs are
/// what says walker.
std::vector<ShapePart> author_walker() {
    std::vector<ShapePart> parts;
    // Slice H1: the hips stand at 1.7 m (were 1.0).
    // The legs are NOT here (slice G): a leg that walks is different every frame, and this list is
    // written once. `walker_legs` makes them, and the mesh emits them before these parts.
    constexpr float kHips = 1.7F;
    // The body, 1.0 m by 0.76 m by 0.68 m, standing on the hips at 1.7 m; its front face is the gun
    // arm's root (scene.hpp's kWalkerGunBottom/Top ride 0.2/0.36 m above the hips).
    constexpr float kBodyTop = kHips + 0.68F;
    add_part(parts,
             {{-0.5F, kHips},
              {kWalkerGunFront, kHips},
              {kWalkerGunFront, kHips + 0.5F},
              {0.35F, kBodyTop},
              {-0.35F, kBodyTop},
              {-0.5F, kHips + 0.5F}},
             -0.38F, 0.38F, false, 0.04F, ShapeRole::Hull);
    add_part(parts,
             {{-0.1F, kHips + 0.28F},
              {0.3F, kHips + 0.28F},
              {0.3F, kHips + 0.52F},
              {-0.1F, kHips + 0.52F}},
             -0.39F, 0.39F, false, 0.0F, ShapeRole::Glass, false);
    // The roof panel, 3 cm thick, the machine's top.
    add_part(
        parts,
        {{-0.4F, kBodyTop}, {0.2F, kBodyTop}, {0.2F, kBodyTop + 0.03F}, {-0.4F, kBodyTop + 0.03F}},
        -0.22F, 0.22F, false, 0.0F, ShapeRole::Track);
    return parts;
}

/// THE APC'S SHAPE. The machine seen from its right-hand side, front at the right: a long
/// slab-sided hull 3.0 m long with a sloped nose, on FOUR WHEELS a side (a dark tyre, a clear ring
/// and a metal hub, as the scout's), a row of three firing ports cut through its flank, a
/// team-colour panel along its roof, and, in the TURRET group, a small turret with a light gun that
/// turns to the target. 1.2 m across the wheels' hubs, as a tank is.
std::vector<ShapePart> author_apc() {
    std::vector<ShapePart> parts;
    add_part(parts, {{-1.5F, 0.28F}, {1.5F, 0.28F}, {1.5F, 0.55F}, {0.95F, 0.88F}, {-1.5F, 0.88F}},
             -0.46F, 0.46F, false, 0.04F, ShapeRole::Hull);
    for (const float port : {-1.15F, -0.65F, -0.15F}) {
        add_part(parts,
                 {{port - 0.15F, 0.5F},
                  {port + 0.15F, 0.5F},
                  {port + 0.15F, 0.64F},
                  {port - 0.15F, 0.64F}},
                 -0.47F, 0.47F, false, 0.0F, ShapeRole::Glass, false);
    }
    // The roof panel along the back, 3 cm thick.
    add_part(parts, {{-1.45F, 0.88F}, {-0.7F, 0.88F}, {-0.7F, 0.91F}, {-1.45F, 0.91F}}, -0.2F, 0.2F,
             false, 0.0F, ShapeRole::Track);

    constexpr float kWheelRadius = 0.28F;
    constexpr float kRingRadius = 0.16F;
    constexpr float kHubRadius = 0.065F;
    for (const float along : {-1.05F, -0.35F, 0.35F, 1.05F}) {
        add_part(parts, ngon(along, kWheelRadius, kWheelRadius, 12), 0.4F, 0.58F, true, 0.02F,
                 ShapeRole::Track);
        add_part(parts, ngon(along, kWheelRadius, kRingRadius, 8), 0.39F, 0.59F, true, 0.0F,
                 ShapeRole::Glass, false);
        add_part(parts, ngon(along, kWheelRadius, kHubRadius, 6), 0.4F, 0.6F, true, 0.0F,
                 ShapeRole::Metal);
    }

    // THE TURRET GROUP: a housing 1.0 m long and 0.68 m across with its roof front raked, and the
    // gun, 8 cm across, level from its front face to 1.25 m.
    add_part(parts, {{-0.6F, 0.85F}, {0.4F, 0.85F}, {0.4F, 0.98F}, {0.25F, 1.1F}, {-0.6F, 1.1F}},
             -0.34F, 0.34F, false, 0.03F, ShapeRole::Hull, true, ShapeGroup::Turret);
    add_part(parts, {{0.4F, 0.92F}, {1.25F, 0.92F}, {1.25F, 1.0F}, {0.4F, 1.0F}}, -0.04F, 0.04F,
             false, 0.01F, ShapeRole::Track, true, ShapeGroup::Turret);
    return parts;
}

/// THE BOMBER'S SHAPE. The machine seen from its right-hand side, front at the right: a long
/// FUSELAGE with a dark nosecone and a canopy cut through both flanks, a tall swept tail fin, a
/// broad WING slung low across it and a BOMB hung under the wing on the ground line, with its fins.
/// A part is a side profile swept across, so the wing's span is its lateral extent: 2.4 m, twice a
/// tank's half-width, the widest profile on the ground. A thin team-colour stripe lies along the
/// spine, and the fin is the highest point, 1.1 m, under the health bar's 1.2 m anchor. Nothing on
/// it aims: the bomb leaves it by the sim's lob and it carries no gun, so every part is Hull.
std::vector<ShapePart> author_bomber() {
    std::vector<ShapePart> parts;
    constexpr float kBelly = 0.38F;
    constexpr float kBomberTail = -1.196682F; ///< authored; the 0.16 cut leaves the tail at -1.05
    constexpr float kSpine = 0.8F;
    // The fuselage, 0.6 m across, its tail rising to the fin.
    add_part(
        parts,
        {{kBomberTail, 0.5F}, {-0.85F, kBelly}, {0.7F, kBelly}, {0.7F, kSpine}, {-0.6F, kSpine}},
        -0.3F, 0.3F, false, 0.05F, ShapeRole::Hull);
    // The canopy: a window wrapped through both flanks, a roof 8 cm thick over it.
    add_part(parts, {{0.05F, 0.6F}, {0.55F, 0.6F}, {0.42F, 0.72F}, {0.05F, 0.72F}}, -0.31F, 0.31F,
             false, 0.0F, ShapeRole::Glass, false);
    // The nosecone, dark, 0.44 m across, running down to a blunt tip.
    add_part(parts, {{0.7F, kBelly}, {1.35F, 0.46F}, {1.35F, 0.6F}, {0.7F, kSpine}}, -0.22F, 0.22F,
             false, 0.04F, ShapeRole::Track);
    // The tail fin, 8 cm thick and swept back, rooted in the fuselage.
    add_part(parts, {{-0.95F, 0.48F}, {-0.4F, 0.68F}, {-0.62F, 1.1F}, {-1.0F, 1.1F}}, -0.04F, 0.04F,
             false, 0.01F, ShapeRole::Metal);
    // The wing, slung low: 2.4 m of span, 1.1 m of chord at the root, standing 10 cm under the
    // belly.
    add_part(parts, {{-0.6F, 0.28F}, {0.5F, 0.28F}, {0.3F, 0.44F}, {-0.6F, 0.44F}}, -1.2F, 1.2F,
             false, 0.04F, ShapeRole::Hull);
    // The bomb: a dark rounded body 0.32 m across hung under the wing, standing on the ground line,
    // and its tail fins.
    add_part(parts, stadium(-0.35F, 0.3F, 0.0F, 0.32F, 4), -0.16F, 0.16F, false, 0.03F,
             ShapeRole::Track);
    add_part(parts, {{-0.5F, 0.02F}, {-0.28F, 0.16F}, {-0.28F, 0.2F}, {-0.5F, 0.3F}}, -0.12F, 0.12F,
             false, 0.005F, ShapeRole::Track);
    // The team-colour stripe along the spine, 3 cm thick.
    add_part(parts,
             {{-0.5F, kSpine}, {0.0F, kSpine}, {0.0F, kSpine + 0.03F}, {-0.5F, kSpine + 0.03F}},
             -0.15F, 0.15F, false, 0.0F, ShapeRole::Track);
    return parts;
}

/// A wheel on both flanks, as the scout's and the APC's: a dark tyre, a clear ring and a metal hub.
/// It stands on the ground line at `along`.
void add_wheel(std::vector<ShapePart>& parts, float along, float radius) {
    add_part(parts, ngon(along, radius, radius, 12), 0.4F, 0.58F, true, 0.02F, ShapeRole::Track);
    add_part(parts, ngon(along, radius, radius * 0.57F, 8), 0.39F, 0.59F, true, 0.0F,
             ShapeRole::Glass, false);
    add_part(parts, ngon(along, radius, radius * 0.23F, 6), 0.4F, 0.6F, true, 0.0F,
             ShapeRole::Metal);
}

/// THE TUG'S SHAPE. The machine seen from its right-hand side, front at the right: a short, low
/// wheeled tractor, 2.4 m long and 0.9 m across, with a cab over its front axle and a tow hitch
/// standing out behind. Nothing on it aims, so every part is Hull.
std::vector<ShapePart> author_tug() {
    std::vector<ShapePart> parts;
    add_part(parts, {{-1.2F, 0.3F}, {1.1F, 0.3F}, {1.1F, 0.62F}, {-1.2F, 0.62F}}, -0.45F, 0.45F,
             false, 0.04F, ShapeRole::Hull);
    add_part(parts, {{0.1F, 0.62F}, {0.95F, 0.62F}, {0.85F, 1.1F}, {0.2F, 1.1F}}, -0.4F, 0.4F,
             false, 0.04F, ShapeRole::Hull);
    // The cab's glass, wrapped through both flanks.
    add_part(parts, {{0.3F, 0.8F}, {0.8F, 0.8F}, {0.78F, 1.0F}, {0.32F, 1.0F}}, -0.41F, 0.41F,
             false, 0.0F, ShapeRole::Glass, false);
    // The tow hitch: a bare metal bar, 12 cm across, reaching 25 cm behind the chassis.
    add_part(parts, {{-1.45F, 0.4F}, {-1.2F, 0.4F}, {-1.2F, 0.55F}, {-1.45F, 0.55F}}, -0.06F, 0.06F,
             false, 0.01F, ShapeRole::Metal);
    for (const float along : {-0.7F, 0.6F}) {
        add_wheel(parts, along, 0.3F);
    }
    return parts;
}

/// THE CONSTRUCTOR'S SHAPE. The machine seen from its right-hand side, front at the right: the
/// scavenger's tracked pods under a flat deck, a cab at the front with a raked windscreen, and a
/// slim CRANE BOOM rising from the deck behind the cab and leaning forward. It raises nothing yet:
/// the buildings a constructor places need a placement feature that does not exist.
std::vector<ShapePart> author_constructor() {
    std::vector<ShapePart> parts;
    add_part(parts, stadium(-1.25F, 1.25F, 0.0F, 0.45F, 6), 0.36F, 0.6F, true, 0.04F,
             ShapeRole::Track);
    add_part(parts, {{-1.2F, 0.4F}, {1.15F, 0.4F}, {1.15F, 0.75F}, {-1.2F, 0.75F}}, -0.4F, 0.4F,
             false, 0.04F, ShapeRole::Hull);
    add_part(parts, {{0.3F, 0.75F}, {1.15F, 0.75F}, {1.15F, 1.1F}, {0.9F, 1.3F}, {0.3F, 1.3F}},
             -0.4F, 0.4F, false, 0.04F, ShapeRole::Hull);
    add_part(parts, {{0.7F, 0.95F}, {1.1F, 0.95F}, {0.95F, 1.2F}, {0.7F, 1.2F}}, -0.41F, 0.41F,
             false, 0.0F, ShapeRole::Glass, false);
    // The boom, 12 cm across, from the deck's rear to a tip 1.4 m up.
    add_part(parts, {{-1.0F, 0.75F}, {-0.8F, 0.75F}, {0.1F, 1.4F}, {-0.1F, 1.4F}}, -0.06F, 0.06F,
             false, 0.01F, ShapeRole::Metal);
    return parts;
}

/// THE SERVICE TRUCK'S SHAPE. The machine seen from its right-hand side, front at the right: a
/// 2.9 m box truck on three axles, its cab raked at the front and its box carrying a CROSS cut
/// through both flanks, the sign of a repair vehicle. It repairs nothing yet.
std::vector<ShapePart> author_service_truck() {
    std::vector<ShapePart> parts;
    add_part(parts, {{-1.5F, 0.3F}, {1.4F, 0.3F}, {1.4F, 0.6F}, {-1.5F, 0.6F}}, -0.46F, 0.46F,
             false, 0.04F, ShapeRole::Hull);
    add_part(parts, {{0.7F, 0.6F}, {1.4F, 0.6F}, {1.4F, 0.85F}, {1.1F, 1.2F}, {0.7F, 1.2F}}, -0.44F,
             0.44F, false, 0.04F, ShapeRole::Hull);
    add_part(parts, {{0.95F, 0.85F}, {1.38F, 0.85F}, {1.15F, 1.12F}, {0.95F, 1.12F}}, -0.45F, 0.45F,
             false, 0.0F, ShapeRole::Glass, false);
    add_part(parts, {{-1.45F, 0.6F}, {0.6F, 0.6F}, {0.6F, 1.2F}, {-1.45F, 1.2F}}, -0.46F, 0.46F,
             false, 0.04F, ShapeRole::Hull);
    // The cross, two bars 8 cm wide.
    add_part(parts, {{-0.75F, 0.72F}, {-0.65F, 0.72F}, {-0.65F, 1.08F}, {-0.75F, 1.08F}}, -0.47F,
             0.47F, false, 0.0F, ShapeRole::Glass, false);
    add_part(parts, {{-0.93F, 0.86F}, {-0.47F, 0.86F}, {-0.47F, 0.94F}, {-0.93F, 0.94F}}, -0.47F,
             0.47F, false, 0.0F, ShapeRole::Glass, false);
    for (const float along : {-1.0F, -0.4F, 1.05F}) {
        add_wheel(parts, along, 0.3F);
    }
    return parts;
}

/// THE SERVICE POD'S SHAPE. The machine seen from its right-hand side, front at the right: a small
/// rounded capsule, 1.6 m long and 0.8 m across, with a canopy cut through both flanks, standing on
/// two skids. The smallest hull in the game. It repairs nothing yet.
std::vector<ShapePart> author_service_pod() {
    std::vector<ShapePart> parts;
    add_part(parts, stadium(-0.8F, 0.8F, 0.25F, 0.85F, 5), -0.4F, 0.4F, false, 0.05F,
             ShapeRole::Hull);
    add_part(parts, {{0.1F, 0.5F}, {0.65F, 0.5F}, {0.55F, 0.72F}, {0.1F, 0.72F}}, -0.41F, 0.41F,
             false, 0.0F, ShapeRole::Glass, false);
    // Two skids 10 cm across, 8 cm thick, each on a strut from the hull.
    add_part(parts, {{-0.7F, 0.0F}, {0.7F, 0.0F}, {0.7F, 0.08F}, {-0.7F, 0.08F}}, 0.25F, 0.35F,
             true, 0.01F, ShapeRole::Track);
    for (const float along : {-0.4F, 0.4F}) {
        add_part(parts,
                 {{along - 0.05F, 0.08F},
                  {along + 0.05F, 0.08F},
                  {along + 0.05F, 0.28F},
                  {along - 0.05F, 0.28F}},
                 0.25F, 0.35F, true, 0.01F, ShapeRole::Metal);
    }
    return parts;
}

/// THE DEFENCE TURRET'S SHAPE, shared by the Turret and the DeployedTurret it becomes. The machine
/// seen from its right-hand side, front at the right: no wheel and no track anywhere. In the Hull
/// group, turned by the heading, a FLARED BASE standing on the ground and a PEDESTAL on it, 1.1 m
/// up; in the TURRET group, turned by the aim, a DOME with its vision slit cut across it and a whip
/// aerial on top, 1.8 m, which is the height the picker tests. The GUN is not here: it elevates
/// when a player drives the turret, so the mesh and the plate add it from the numbers in scene.hpp
/// (`kTurretGunFront`, `kTurretBarrelTip`, ...). 1.2 m across the base, the tank's width.
std::vector<ShapePart> author_turret() {
    std::vector<ShapePart> parts;
    constexpr float kBaseHalf = kHullHalfMetres;
    constexpr float kTurretFoot
        = 0.785978F; ///< authored; the 0.16 cut leaves the base at 0.7 either side
    constexpr float kPedestalTop = 1.1F;
    add_part(parts, {{-kTurretFoot, 0.0F}, {kTurretFoot, 0.0F}, {0.5F, 0.45F}, {-0.5F, 0.45F}},
             -kBaseHalf, kBaseHalf, false, 0.05F, ShapeRole::Hull);
    add_part(parts, {{-0.5F, 0.4F}, {0.5F, 0.4F}, {0.5F, kPedestalTop}, {-0.5F, kPedestalTop}},
             -0.5F, 0.5F, false, 0.04F, ShapeRole::Hull);

    // The dome: a six-sided hood 0.9 m across, its front face the barrel's root.
    add_part(parts,
             {{-0.5F, kPedestalTop},
              {kTurretGunFront, kPedestalTop},
              {kTurretGunFront, 1.4F},
              {0.3F, 1.7F},
              {-0.3F, 1.7F},
              {-0.5F, 1.4F}},
             -0.45F, 0.45F, false, 0.05F, ShapeRole::Metal, true, ShapeGroup::Turret);
    // The vision slit, wrapped through both flanks, 1 cm proud of the dome's side.
    add_part(parts, {{-0.25F, 1.36F}, {0.2F, 1.36F}, {0.2F, 1.44F}, {-0.25F, 1.44F}}, -0.46F, 0.46F,
             false, 0.0F, ShapeRole::Glass, false, ShapeGroup::Turret);
    // The whip aerial on the crown, dark, 8 cm across, standing a decimetre over it.
    add_part(parts, {{-0.2F, 1.68F}, {-0.12F, 1.68F}, {-0.12F, 1.8F}, {-0.2F, 1.8F}}, -0.04F, 0.04F,
             false, 0.01F, ShapeRole::Track, true, ShapeGroup::Turret);
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

std::vector<std::array<float, 2>> rounded_profile(std::vector<std::array<float, 2>> profile) {
    // Two passes: the first cuts every corner over 60 degrees (a 90 becomes two 45s); the second
    // cuts again the few the first left over 66 -- a corner of more than 132 degrees (the bomber
    // wing's tip, 141) would otherwise be two of 70 and more, too close to the crease for smooth
    // shading to blend. The tank's glacis foot, 127 degrees, is two 63.7s and stays so, as AT left
    // it.
    constexpr float kFirstPassTurnDegrees = 60.0F;
    constexpr float kSecondPassTurnDegrees = 66.0F;
    Profile corners = counter_clockwise(std::move(profile));
    corners = chamfer_corners(corners, kHullChamfer, kFirstPassTurnDegrees);
    return chamfer_corners(corners, kHullChamfer, kSecondPassTurnDegrees);
}

const std::vector<ShapePart>& shape_of(sim::UnitKind kind) {
    static const std::vector<ShapePart> scavenger = author_scavenger();
    static const std::vector<ShapePart> tank = author_tank();
    static const std::vector<ShapePart> hover_tank = author_hover_tank();
    static const std::vector<ShapePart> scout = author_scout();
    static const std::vector<ShapePart> msl_scout = author_msl_scout();
    static const std::vector<ShapePart> rocket_tank = author_rocket_tank();
    static const std::vector<ShapePart> assault_tank = author_assault_tank();
    static const std::vector<ShapePart> mortar_bike = author_mortar_bike();
    static const std::vector<ShapePart> walker = author_walker();
    static const std::vector<ShapePart> apc = author_apc();
    static const std::vector<ShapePart> bomber = author_bomber();
    static const std::vector<ShapePart> turret = author_turret();
    static const std::vector<ShapePart> tug = author_tug();
    static const std::vector<ShapePart> constructor = author_constructor();
    static const std::vector<ShapePart> service_truck = author_service_truck();
    static const std::vector<ShapePart> service_pod = author_service_pod();
    static const std::vector<ShapePart> none; // a kind with no authored shape has an empty one
    switch (kind) {
    case sim::UnitKind::Scavenger:
        return scavenger;
    case sim::UnitKind::Tank:
        return tank;
    case sim::UnitKind::HoverTank:
        return hover_tank;
    case sim::UnitKind::Scout:
        return scout;
    case sim::UnitKind::MslScout:
        return msl_scout;
    case sim::UnitKind::RocketTank:
        return rocket_tank;
    case sim::UnitKind::AssaultTank:
        return assault_tank;
    case sim::UnitKind::MortarBike:
        return mortar_bike;
    case sim::UnitKind::Walker:
        return walker;
    case sim::UnitKind::Apc:
        return apc;
    case sim::UnitKind::Bomber:
        return bomber;
    case sim::UnitKind::Turret:
    case sim::UnitKind::DeployedTurret: // the same machine: it only becomes deployed
        return turret;
    case sim::UnitKind::Tug:
        return tug;
    case sim::UnitKind::Constructor:
        return constructor;
    case sim::UnitKind::ServiceTruck:
        return service_truck;
    case sim::UnitKind::ServicePod:
        return service_pod;
    default:
        return none;
    }
}

std::vector<ShapePart> walker_legs(const LegPose& front, const LegPose& rear) {
    // The authored stance, mid-stride: the right leg the front one with its foot at +H from the
    // hip, the left the rear one at -H. Across: the leg -0.40..-0.24 / 0.24..0.40, the foot
    // 0.20..0.44.
    constexpr float kLegLo = 0.24F;
    constexpr float kLegHi = 0.4F;
    constexpr float kFootLo = 0.2F;
    constexpr float kFootHi = 0.44F;
    constexpr float kHipTop = 1.75F; // the thigh's top, inside the body: it does not move
    constexpr float kAnkle = 0.2F;   // from here down the foot is one rigid sole
    constexpr float kThighFoot = 1.0F;
    constexpr float kKnee = 1.05F;
    // Between the hip and the ankle a point moves with its share of the foot's travel.
    const auto moved = [&](const Profile& authored, const LegPose& pose, float authored_along) {
        Profile out;
        out.reserve(authored.size());
        const float dx = pose.along - authored_along;
        for (const auto& corner : authored) {
            const float share = std::clamp((kHipTop - corner[1]) / (kHipTop - kAnkle), 0.0F, 1.0F);
            out.push_back({corner[0] + (dx * share), corner[1] + (pose.lift * share)});
        }
        return out;
    };
    std::vector<ShapePart> parts;
    const float h = kWalkerStrideHalfMetres;
    add_part(
        parts,
        moved({{-0.35F, kHipTop}, {-0.15F, kHipTop}, {-0.33F, kThighFoot}, {-0.55F, kThighFoot}},
              rear, -h),
        -kLegHi, -kLegLo, false, 0.02F, ShapeRole::Track);
    add_part(parts,
             moved({{-0.55F, kKnee}, {-0.33F, kKnee}, {-0.5F, kAnkle}, {-0.72F, kAnkle}}, rear, -h),
             -kLegHi, -kLegLo, false, 0.02F, ShapeRole::Track);
    add_part(parts,
             moved({{-1.0F, 0.0F}, {-0.3F, 0.0F}, {-0.3F, kAnkle}, {-1.0F, kAnkle}}, rear, -h),
             -kFootHi, -kFootLo, false, 0.03F, ShapeRole::Track);
    add_part(parts,
             moved({{0.15F, kHipTop}, {0.35F, kHipTop}, {0.5F, kThighFoot}, {0.28F, kThighFoot}},
                   front, h),
             kLegLo, kLegHi, false, 0.02F, ShapeRole::Track);
    add_part(parts,
             moved({{0.28F, kKnee}, {0.5F, kKnee}, {0.72F, kAnkle}, {0.5F, kAnkle}}, front, h),
             kLegLo, kLegHi, false, 0.02F, ShapeRole::Track);
    add_part(parts, moved({{0.3F, 0.0F}, {1.0F, 0.0F}, {1.0F, kAnkle}, {0.3F, kAnkle}}, front, h),
             kFootLo, kFootHi, false, 0.03F, ShapeRole::Track);
    return parts;
}

const std::vector<ShapePart>& silhouette_of(sim::UnitKind kind) {
    // The Walker's plate and extents are its legs at the authored stance (phase 0) under its body.
    static const std::vector<ShapePart> walker = [] {
        std::vector<ShapePart> parts = walker_legs(LegPose{kWalkerStrideHalfMetres, 0.0F},
                                                   LegPose{-kWalkerStrideHalfMetres, 0.0F});
        const std::vector<ShapePart>& body = shape_of(sim::UnitKind::Walker);
        parts.insert(parts.end(), body.begin(), body.end());
        return parts;
    }();
    return kind == sim::UnitKind::Walker ? walker : shape_of(kind);
}

bool shape_covers(sim::UnitKind kind, float along, float up) {
    bool lit = false;
    for (const ShapePart& part : silhouette_of(kind)) {
        if (inside(part.profile, along, up)) {
            lit = part.lit;
        }
    }
    return lit;
}

ShapeExtent shape_extent(sim::UnitKind kind) {
    const std::vector<ShapePart>& parts = silhouette_of(kind);
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
