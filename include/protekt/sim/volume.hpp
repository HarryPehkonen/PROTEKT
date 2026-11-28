#pragma once

#include <cstdint>

#include "protekt/sim/fixed.hpp"
#include "protekt/sim/units.hpp"

namespace protekt::sim {

/// THE VOLUME (M5). A solid thing is THREE NUMBERS: an EXTENT (which tiles, a `SolidMask`), a BASE
/// and a TOP (the heights it occupies, in height units above the local ground:
/// `kHeightUnitsPerTile` to a tile). The mask of M4 is the volume's EXTENT, not a second idea
/// beside it: a kind that declares a mask has a volume whose extent is that mask, and a kind with
/// none has an EMPTY extent, which a building reads as its drawn box (`drawn_box`) exactly as
/// before.
///
/// What a flat mask cannot say, a volume can: a RAMP to a raised section, a BRIDGE (base above a
/// vehicle's head: a unit passes under, a unit on top drives over), a POST between which you drive,
/// an OVERHANG. And "may I be here" has a height in it (`volume_stops`): the rule that makes a
/// jump possible at all, because a jumped unit is only clear of the wall if the rule knows it is
/// airborne.
///
/// Two frames, one type. A BUILDING's volume is `volume_of(kind)`, its extent in the building's OWN
/// frame (turned with it, like the mask). TERRAIN's is a `PlacedVolume`: the same type set down on
/// a world tile.
struct Volume {
    SolidMask extent{};
    Fixed base{};
    Fixed top{};
};

/// THE JUMP'S NUMBERS (M5). Harri: "SPEED is what it takes: below a threshold you just bump the
/// obstacle; above it, you clear it." Every one is PROVISIONAL -- he gave the rule, not the
/// figures.
///
/// THE SPEED THRESHOLD: three quarters of a tank's full speed (0.15 tiles a tick, 4.5 a second):
/// the commanded speed the unit must be moving at when the jump is asked for. A tank at full
/// throttle has it, with room to spare, and so does anything quicker; a Walker (half a tank's
/// speed) or a crawling Turret never does, and a tank from a standstill has none. `>=`: exactly the
/// threshold jumps. It is read off the unit's velocity (`Unit::vx`/`vy`), what its last tick's
/// steering asked for, so a run-up of one tick is enough and a strafe alone is not one.
inline constexpr std::int64_t kJumpSpeedRaw = kTankSpeedRaw * 3 / 4;
/// THE LAUNCH: 10 height units a tick up, against `kJumpGravity` of 1 a tick lost to gravity. The
/// body then rises 10, 19, 27, 34 ... to a peak of 55 units (about 1.7 m at 32 units to the metre:
/// more than a tank's whole 1.3 m body, `body_height_of`) on ticks 10 and 11, and lands on the
/// 21st tick (0.7 s). A volume with its top within 55 of the unit's ride height (`kRideHeight`) --
/// so up to 63 above the ground -- can be cleared, and for 14 of those ticks a 40-unit lip can: at
/// a tank's full speed that is about 2.8 tiles of clear flight, at the threshold about 2.1. Every
/// building is far above it (a roof of 238 units or more), so a jump clears walls, lips and kerbs
/// and never a building.
inline constexpr Fixed kJumpLaunch = Fixed::from_raw(10 * Fixed::kOne);
inline constexpr Fixed kJumpGravity = Fixed::from_raw(Fixed::kOne);

/// A volume anchored to the WORLD: its extent's north-west corner is on (`x`, `y`), turned a
/// quarter or not. Scenario data (`Scenario::volumes`): placed by whoever built the scenario, never
/// hashed, like a pool.
struct PlacedVolume {
    int x{};
    int y{};
    bool turned{};
    Volume volume{};
};

/// THE VOLUME OF A BUILDING `kind`: its extent (the kind's mask, empty for the drawn box), from the
/// ground to its ROOF -- the very height a shell must clear (`roof_top`, which reads this).
///
/// THIS IS THE ONE FACT THAT MAKES A BUILDING SOLID (G14). The mover asks it
/// (`building_stops_units`, through `building_solid_at` and `volume_stops`), so a driven move, a
/// strafe, a shove's carry, a jump's landing and the route planner all read it, because they all
/// ask `WorldGround::factor`. The bolt asks it too (`for_each_solid_rect`). G14 gave the recycler
/// its own extent (`kDeployedRecyclerMask`), the plan of the hull it is drawn as. Before that it
/// had the generic box, so the drawing was a second shape that nothing in the sim read. Its ROOF is
/// still the generic one (7.4 m against a 10 m drawn cab). Raising it would put a recycler out of
/// a mortar's reach to lob over, against the owner's lob ruling, so that is reported, not done.
[[nodiscard]] constexpr Volume volume_of(UnitKind kind) noexcept {
    const Fixed body = kind == UnitKind::Factory ? kFactoryBodyHeight : kBodyHeight;
    return Volume{solid_mask_of(kind), Fixed::zero(),
                  Fixed::from_raw(kRideHeight.raw + body.raw * kBuildingFootprintScale)};
}

/// THE ONE HEIGHT RULE: SPAN OVERLAP (G16). A vehicle lifted `lift` above the ground, of body
/// height `body` (`body_height_of(kind)`, its drawn height), occupies the heights
/// [kRideHeight + lift, kRideHeight + lift + body); a volume occupies [base, top). The volume STOPS
/// the vehicle iff the two spans OVERLAP. That is the whole rule, and every shape follows from it:
///   * a wall or a kerb (base zero) stops whatever's feet are below its top, a jumped unit clearing
///     it once its feet rise to the top;
///   * a FLOATING slab (base above zero) lets a vehicle whose head is at or below its base pass
///     UNDER, and stops a taller one -- a bike under, a tank stopped, at the same base;
///   * a V, a pyramid, an upside-down pyramid, the prongs: each tile is one span, asked the same.
/// Nothing in it names a kind: a taller kind is stopped by more because its `body` is larger, and
/// for no other reason. A building (base zero, a roof of 238 or more) stops every grounded unit, as
/// it always did.
///
/// G18: and FROM A FLOOR, `floor` (`span_floor_of(kind)`): where the span starts before any lift.
/// The ride height for every ground kind -- the default, every caller before G18 -- and the flight
/// ALTITUDE for an aircraft. The same rule; an aircraft's span just starts higher.
[[nodiscard]] constexpr bool volume_stops(const Volume& volume, Fixed lift, Fixed body,
                                          Fixed floor = kRideHeight) noexcept {
    const std::int64_t feet = static_cast<std::int64_t>(floor.raw) + lift.raw;
    const std::int64_t head = feet + body.raw;
    return feet < volume.top.raw && head > volume.base.raw;
}

/// THE MAP'S SOLID GROUND AS A SPAN (G18). A `[solid]` tile (`world::ground_solid`; the Proving
/// Ground's rockfall) was the one thing that stopped a unit with NO height at all: whatever the
/// unit's lift or body, it was a wall. That cannot be asked by the one height rule, and an aircraft
/// must cross it ("the rockfall"), so it is given a span: from the ground to the TALLEST ROOF IN
/// THE GAME, the Factory's (`volume_of(Factory).top`, 584 units, 18.25 m). It is drawn as no taller
/// than its terrain, so the number is not a drawing: it is "as solid as the tallest building",
/// which still stops every ground unit at every lift it reaches on an authored map (a jump peaks 55
/// above the ride height; the Proving Ground's steepest step between neighbouring corners is 170
/// units). Its extent is unused: the raster says which tiles.
inline constexpr Volume kSolidGround{SolidMask{}, Fixed::zero(), volume_of(UnitKind::Factory).top};

/// THE ALTITUDE CLEARS THE TALLEST THING (G18): every aircraft's span starts above the Factory's
/// roof, which is the solid ground's top too. Raise a roof past it and this fails, not a player.
static_assert(span_floor_of(UnitKind::Bomber).raw > kSolidGround.top.raw,
              "the bomber must fly above the tallest authored thing: restate the altitude");

/// Whether the TERRAIN volume `placed` is solid on world tile (`tx`, `ty`): its extent, laid on the
/// world.
[[nodiscard]] constexpr bool placed_solid_at(const PlacedVolume& placed, int tx, int ty) noexcept {
    return placed.volume.extent.solid_placed(placed.x, placed.y, placed.turned, tx, ty);
}

} // namespace protekt::sim
