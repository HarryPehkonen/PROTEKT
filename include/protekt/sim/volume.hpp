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
/// roughly its own body's height again) on ticks 10 and 11, and lands on the 21st tick (0.7 s). A
/// volume with its top within 55 of the unit's ride height (`kRideHeight`) -- so up to 63 above the
/// ground -- can be cleared, and for 14 of those ticks a 40-unit lip can: at a tank's full speed
/// that is about 2.8 tiles of clear flight, at the threshold about 2.1. Every building is far above
/// it (a roof of 238 units or more), so a jump clears walls, lips and kerbs and never a building.
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

/// THE ONE HEIGHT RULE. A vehicle whose body is lifted `lift` above the ground occupies the heights
/// from its ride height to its ride height plus its body (`kRideHeight`, `kBodyHeight`); a volume
/// STOPS it when the two ranges overlap. A grounded unit (`lift` zero) is stopped by every volume
/// that starts below its roof, which is every building there is -- so a unit on the ground behaves
/// as it always did.
[[nodiscard]] constexpr bool volume_stops(const Volume& volume, Fixed lift) noexcept {
    const std::int64_t feet = static_cast<std::int64_t>(kRideHeight.raw) + lift.raw;
    const std::int64_t head = feet + kBodyHeight.raw;
    return feet < volume.top.raw && head > volume.base.raw;
}

/// Whether the TERRAIN volume `placed` is solid on world tile (`tx`, `ty`): its extent, laid on the
/// world.
[[nodiscard]] constexpr bool placed_solid_at(const PlacedVolume& placed, int tx, int ty) noexcept {
    return placed.volume.extent.solid_placed(placed.x, placed.y, placed.turned, tx, ty);
}

} // namespace protekt::sim
