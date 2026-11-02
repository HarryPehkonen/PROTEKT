#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace protekt::game {

/// THE SCAVENGER'S SHAPE, written once. Harri's idea (from a BZ2 crop): the vehicles should look
/// like their icons. Modelling a mesh to match a drawing keeps two descriptions in step by hand, so
/// there is ONE description instead, and both are derived from it:
///  * the WORLD MESH (`build_scavenger` in scene.cpp) extrudes every part across its lateral
///    extent, bevelled, so the machine has a body and not a cardboard cut-out;
///  * the 72 px PLATE (`kind_icon`'s scavenger) is the side-on PROJECTION of the same parts: a cell
///    is lit where `scavenger_covers` says the silhouette is.
/// Neither holds a number of its own about what the machine looks like, so they cannot disagree.
/// The shape's numbers are in `src/game/scavenger_shape.cpp` and nowhere else.
///
/// Frame, in metres: `along` is forward (+) from the unit's centre, `up` is over the ground,
/// `across` is the lateral offset from the centreline. The scavenger and the harvester are one kind
/// that differs by faction; this is the HUMAN look and the plate's, which is shared by both.

/// What a part is made of: it picks the mesh's tone and tile, never the silhouette.
enum class ShapeRole : std::uint8_t {
    Hull,  ///< the army's camo, as a tank's hull
    Track, ///< dark, in the side colour, as a tank's tracks
    Metal, ///< the plain turret metal, in the army's pale tint
    Load,  ///< scrap in the hopper: darkened camo, rough
    Glass, ///< a CUT: clears the plate, and is a dark inlay proud of the flanks on the mesh
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
};

/// The parts in PAINT ORDER: later parts paint over earlier ones, on the plate and in the mesh.
[[nodiscard]] const std::vector<ShapePart>& scavenger_shape();

/// The side-on silhouette at a point: true where the machine is. Replays `scavenger_shape()` in
/// order, a lit part lighting the point it covers and a cut clearing it. The plate's one source.
[[nodiscard]] bool scavenger_covers(float along, float up);

/// How far the machine reaches, in metres. `along_min`..`along_max` and `up_max` are of the lit
/// parts; `across_max` is the farthest any part stands from the centreline, so twice it is the
/// machine's width.
struct ShapeExtent {
    float along_min;
    float along_max;
    float up_max;
    float across_max;
};
[[nodiscard]] ShapeExtent scavenger_extent();

} // namespace protekt::game
