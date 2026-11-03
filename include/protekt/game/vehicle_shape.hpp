#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "protekt/sim/units.hpp"

namespace protekt::game {

/// ONE SHAPE PER KIND, written once. Harri's idea (from a BZ2 crop): the vehicles should look like
/// their icons. Modelling a mesh to match a drawing keeps two descriptions in step by hand, so
/// there is ONE description per kind instead, and both are derived from it:
///  * the WORLD MESH (`build_scavenger`, `build_facing_tank` in scene.cpp) extrudes every part
///    across its lateral extent, bevelled, so the machine has a body and not a cardboard cut-out;
///  * the 72 px PLATE (`kind_icon`'s scavenger and tank) is the side-on PROJECTION of the same
///    parts: a cell is lit where `shape_covers` says the silhouette is.
/// Neither holds a number of its own about what the machine looks like, so they cannot disagree.
/// The shapes' numbers are in `src/game/vehicle_shape.cpp` and nowhere else.
///
/// Kinds are converted one batch at a time. A kind with NO authored shape has an EMPTY one:
/// `shape_of` returns an empty list, `shape_covers` is false everywhere and `shape_extent` is all
/// zero. Empty means "not converted": that kind's builder and plate keep their own recipes, and the
/// callers test `shape_of(kind).empty()` to know. Currently authored: every vehicle -- the
/// Scavenger (the human hauler; the alien harvester is the same kind with its own organic mesh and
/// shares the plate), the Tank, the Hover Tank, the Scout, the Missile Scout, the Rocket Tank, the
/// Assault Tank, the Mortar Bike, the Walker, the Apc, the Bomber, and the defence Turret (one
/// shape for the Turret and the DeployedTurret it becomes). The buildings and the recycler keep
/// their own.
///
/// Frame, in metres: `along` is forward (+) from the unit's centre, `up` is over the ground,
/// `across` is the lateral offset from the centreline.

/// What a part is made of: it picks the mesh's tone and tile, never the silhouette.
enum class ShapeRole : std::uint8_t {
    Hull,  ///< the army's camo, as a tank's hull
    Track, ///< dark, in the side colour, as a tank's tracks
    Metal, ///< the plain turret metal, in the army's pale tint
    Load,  ///< scrap in the hopper: darkened camo, rough
    Glass, ///< a CUT: clears the plate, and is a dark inlay proud of the flanks on the mesh
};

/// Which part of the machine a part turns with. The mesh turns a `Turret` part by the turret's
/// bearing and a `Hull` part by the heading; the side-on plate shows one pose, so it draws the
/// turret level, at zero, which is simply the profile as authored.
enum class ShapeGroup : std::uint8_t {
    Hull,
    Turret,
};

/// One part of the machine. `profile` is a CONVEX polygon of (along, up) points, any winding; the
/// part is that profile swept across `[across_lo, across_hi]` (and again across the reflected range
/// when `mirrored`), its edges chamfered by `bevel` metres. A part with `lit` false is a cut: on
/// the plate it clears the cells it covers, which is how a window or a wheel's gap is made.
struct ShapePart {
    std::vector<std::array<float, 2>> profile;
    float across_lo;
    float across_hi;
    bool mirrored;
    float bevel;
    ShapeRole role;
    bool lit;
    ShapeGroup group = ShapeGroup::Hull;
};

/// The parts of `kind` in PAINT ORDER: later parts paint over earlier ones, on the plate and in the
/// mesh. EMPTY for a kind with no authored shape (see the top of this file), never another kind's.
[[nodiscard]] const std::vector<ShapePart>& shape_of(sim::UnitKind kind);

/// The side-on silhouette at a point: true where the machine is. Replays `shape_of(kind)` in order,
/// a lit part lighting the point it covers and a cut clearing it. The plate's one source. False
/// everywhere for a kind with no shape.
[[nodiscard]] bool shape_covers(sim::UnitKind kind, float along, float up);

/// How far the machine reaches, in metres. `along_min`..`along_max` and `up_max` are of the lit
/// parts; `across_max` is the farthest any part stands from the centreline, so twice it is the
/// machine's width. All zero for a kind with no shape.
struct ShapeExtent {
    float along_min;
    float along_max;
    float up_max;
    float across_max;
};
[[nodiscard]] ShapeExtent shape_extent(sim::UnitKind kind);

} // namespace protekt::game
