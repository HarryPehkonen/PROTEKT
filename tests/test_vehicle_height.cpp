#include "protekt/game/vehicle_shape.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <iostream>
#include <vector>

/// SLICE G16: HOW TALL IS A VEHICLE. Harri: "That also gives birth to the desire to create, for
/// example, an upside-down pyramid shape. A tall vehicle can't go through, but a shorter one can."
///
/// A unit's body occupies the heights from its ride height to its ride height plus ITS OWN body
/// height; a volume occupies its base to its top; it stops the unit where the two spans overlap.
/// Every measurement here is made by DRIVING: a unit ordered across a floating slab, and whether it
/// came out the far side. The reference the order is checked against is the DRAWING
/// (`game::shape_extent`, the top of the machine's authored shape), never the sim's own table, so
/// a sim height that drifted from the model it is drawn as is caught here.
///
/// The rig is test_volume's: seed 42's flat strip (x 56 .. 82), a slab one tile deep and 27 wide on
/// row kSlabY, the unit 1.5 tiles north of it on column 69, ordered to a goal on the far side.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;
constexpr int kSlabY = 29;
constexpr int kSlabLeft = 56;
constexpr int kSlabWidth = 27;
constexpr int kStartX = 69;
constexpr int kStartY = 27;
constexpr int kGoalY = 34;
constexpr int kRunTicks = 90;
/// How thick a slab is: it is only its BASE that a unit under it meets.
constexpr std::int32_t kSlabThickRaw = 20 * Fixed::kOne;
/// Height units to the metre (32): a tile is 2 m and `kHeightUnitsPerTile` (64) to a tile.
constexpr double kUnitsPerMetre = static_cast<double>(kHeightUnitsPerTile) / kTileMetres;

[[nodiscard]] bool open_ground() {
    for (int y = 22; y <= 35; ++y) {
        for (int x = 56; x <= 82; ++x) {
            if (world::sample(kSeed, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

/// A slab whose underside is `base_raw` above the ground, one tile deep along row kSlabY.
[[nodiscard]] std::vector<PlacedVolume> slab(std::int32_t base_raw) {
    std::vector<PlacedVolume> out;
    for (int x = kSlabLeft; x < kSlabLeft + kSlabWidth; x += 8) {
        const int width = std::min(8, kSlabLeft + kSlabWidth - x);
        const SolidMask row{width, 1, (std::uint64_t{1} << static_cast<unsigned>(width)) - 1};
        out.push_back(PlacedVolume{
            x, kSlabY, false,
            Volume{row, Fixed::from_raw(base_raw), Fixed::from_raw(base_raw + kSlabThickRaw)}});
    }
    return out;
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

[[nodiscard]] Sim rig(UnitKind kind, std::int32_t base_raw) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(kind, Side::Player, kStartX, kStartY),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    scenario.volumes = slab(base_raw);
    return Sim::create(scenario);
}

/// A drive at the slab: where the unit was each tick, and whether it went UNDER it -- came out on
/// the far side (`kSlabY + 1`) without ever leaving the slab's width (a tile in from either end),
/// so a unit that drove round the end of the slab is not counted as one that passed under it.
struct Pass {
    bool under{};
    std::vector<std::array<double, 2>> track;
};

[[nodiscard]] Pass drive(UnitKind kind, std::int32_t base_raw) {
    Sim sim = rig(kind, base_raw);
    const UnitId id = sim.units().at(0).id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kStartX), Fixed::from_int(kGoalY)}});
    Pass pass{};
    bool inside = true; // never left the slab's width: a way round its end is not a way under it
    for (int t = 0; t < kRunTicks; ++t) {
        sim.step();
        const Unit& u = *sim.find(id);
        const double x = static_cast<double>(u.x.raw) / kTile;
        const double y = static_cast<double>(u.y.raw) / kTile;
        pass.track.push_back({x, y});
        inside = inside && x > kSlabLeft + 1.0 && x < kSlabLeft + kSlabWidth - 1.0;
        if (y > kSlabY + 1.0) {
            pass.under = inside;
            break;
        }
    }
    return pass;
}

/// The LOWEST slab base, in raw height units, that `kind` drives under: a bisection over the base,
/// every probe a real drive. Stopped at base zero (a kerb) and passing at 128 (two tiles up) are
/// asserted, so the answer is bracketed and the drive, not the search, decides it.
[[nodiscard]] std::int32_t lowest_base_passed_under(UnitKind kind) {
    std::int32_t stopped = 0;
    std::int32_t passed = 128 * Fixed::kOne;
    EXPECT_FALSE(drive(kind, stopped).under) << "a slab on the ground is a kerb";
    EXPECT_TRUE(drive(kind, passed).under) << "a slab two tiles up is passed under by anything";
    while (passed - stopped > 1) {
        const std::int32_t mid = stopped + ((passed - stopped) / 2);
        (drive(kind, mid).under ? passed : stopped) = mid;
    }
    return passed;
}

[[nodiscard]] double units_of_raw(std::int64_t raw) {
    return static_cast<double>(raw) / Fixed::kOne;
}

/// The DRAWN height of `kind`'s machine in height units: the top of its authored shape (metres
/// above the hull's base, which stands at the ride height) at 32 units a metre.
[[nodiscard]] double drawn_height_units(UnitKind kind) {
    return static_cast<double>(game::shape_extent(kind).up_max) * kUnitsPerMetre;
}

[[nodiscard]] const char* name_of(UnitKind kind) {
    switch (kind) {
    case UnitKind::MortarBike:
        return "mortar bike";
    case UnitKind::Apc:
        return "APC";
    case UnitKind::Tank:
        return "tank";
    default:
        return "?";
    }
}

// --- 1. THE HEIGHT LADDER --------------------------------------------------------------------

TEST(VehicleHeight, TheLowestSlabEachPassesUnderFollowsItsDrawnHeight) {
    ASSERT_TRUE(open_ground());
    struct Rung {
        UnitKind kind;
        double drawn;  ///< drawn height, height units
        double lowest; ///< lowest base it drives under, height units
    };
    std::vector<Rung> ladder;
    for (const UnitKind kind : {UnitKind::MortarBike, UnitKind::Apc, UnitKind::Tank}) {
        ladder.push_back(
            Rung{kind, drawn_height_units(kind), units_of_raw(lowest_base_passed_under(kind))});
    }
    std::cout << "[ G16 ] kind          drawn top (m)  drawn (units)  ride+drawn  lowest base "
                 "passed under (units)\n";
    for (const Rung& r : ladder) {
        std::cout << std::format("[ G16 ] {:12}  {:13.5f}  {:13.4f}  {:10.4f}  {:.5f}\n",
                                 name_of(r.kind), r.drawn / kUnitsPerMetre, r.drawn,
                                 units_of_raw(kRideHeight.raw) + r.drawn, r.lowest);
    }
    // Each one passes under exactly what its own drawn body clears: its ride height plus its drawn
    // height, to a millimetre (0.032 units; the drawing is in float metres, the sim in Q16.16).
    for (const Rung& r : ladder) {
        EXPECT_NEAR(r.lowest, units_of_raw(kRideHeight.raw) + r.drawn, 0.032) << name_of(r.kind);
    }
    // Strictly ordered, and in the order of the heights: sorted by drawn height, the lowest base
    // each passes under strictly rises.
    std::vector<Rung> by_height = ladder;
    std::ranges::sort(by_height, {}, &Rung::drawn);
    for (std::size_t i = 1; i < by_height.size(); ++i) {
        EXPECT_LT(by_height.at(i - 1).lowest, by_height.at(i).lowest)
            << name_of(by_height.at(i - 1).kind) << " then " << name_of(by_height.at(i).kind);
    }
}

// --- 2. A FLOATING SLAB: one base, the bike under it and the tank stopped
// -------------------------

TEST(VehicleHeight, OneFloatingSlabTheBikePassesUnderAndTheTankIsStoppedBy) {
    ASSERT_TRUE(open_ground());
    // 46 units (1.44 m) up: above the bike's head (8 + 35.2) and below the tank's (8 + 41.6).
    constexpr std::int32_t kBase = 46 * Fixed::kOne;
    ASSERT_GT(units_of_raw(kBase),
              units_of_raw(kRideHeight.raw) + drawn_height_units(UnitKind::MortarBike));
    ASSERT_LT(units_of_raw(kBase),
              units_of_raw(kRideHeight.raw) + drawn_height_units(UnitKind::Tank));
    const Pass bike = drive(UnitKind::MortarBike, kBase);
    const Pass tank = drive(UnitKind::Tank, kBase);
    for (const auto& [label, pass] : {std::pair{"mortar bike", &bike}, std::pair{"tank", &tank}}) {
        std::cout << std::format("[ G16 ] slab base 46 top 66 on row {}: {} -- {}\n", kSlabY, label,
                                 pass->under ? "PASSED UNDER" : "did not pass under");
        for (std::size_t t = 0; t < pass->track.size(); t += 5) {
            std::cout << std::format("[ G16 ]   tick {:3}  x {:7.3f}  y {:7.3f}\n", t + 1,
                                     pass->track.at(t)[0], pass->track.at(t)[1]);
        }
        const auto& last = pass->track.back();
        std::cout << std::format("[ G16 ]   last ({} ticks)  x {:7.3f}  y {:7.3f}\n",
                                 pass->track.size(), last[0], last[1]);
    }
    EXPECT_TRUE(bike.under) << "the bike is shorter than the slab is high: it drives under";
    EXPECT_FALSE(tank.under) << "the tank is taller: the slab stops it";
}

// --- 3. THE SIM'S HEIGHTS ARE THE DRAWN ONES
// ------------------------------------------------------

TEST(VehicleHeight, EveryAuthoredKindsBodyHeightIsItsDrawnHeight) {
    // The drawing-versus-blocking drift, closed for the height: every kind with an authored shape
    // has the body height the shape is drawn to, to half a millimetre (the table is in whole mm).
    int checked = 0;
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        const game::ShapeExtent extent = game::shape_extent(kind);
        if (extent.up_max == 0.0F) {
            continue; // no authored shape: the recycler and the buildings
        }
        const double sim_m = units_of_raw(body_height_of(kind).raw) / kUnitsPerMetre;
        std::cout << std::format("[ G16 ] kind {:2}: drawn {:.5f} m, sim {:.5f} m\n", k,
                                 extent.up_max, sim_m);
        EXPECT_NEAR(sim_m, static_cast<double>(extent.up_max), 0.0005) << "kind " << k;
        ++checked;
    }
    EXPECT_EQ(checked, 17) << "every vehicle and the turret pair";
    // The recycler is not an authored shape: its 2.5 m is its cab roof at twice the tank's scale.
    EXPECT_EQ(body_height_of(UnitKind::Recycler).raw, height_of_mm(2500).raw);
    EXPECT_EQ(tallest_body().raw, body_height_of(UnitKind::Recycler).raw)
        << "the tallest vehicle is the recycler";
}

TEST(VehicleHeight, TheRuleIsSpanOverlapAndOnlyTheBodyDiffers) {
    // One rule: [8 + lift, 8 + lift + body) against [base, top). Two kinds, the same slab, the
    // same lift: what differs is the body, and nothing else is asked.
    const Volume slab{SolidMask{1, 1, 1}, Fixed::from_int(46), Fixed::from_int(66)};
    const Fixed bike = body_height_of(UnitKind::MortarBike);
    const Fixed tank = body_height_of(UnitKind::Tank);
    EXPECT_FALSE(volume_stops(slab, Fixed::zero(), bike)) << "8 + 35.2 = 43.2 <= 46: under";
    EXPECT_TRUE(volume_stops(slab, Fixed::zero(), tank)) << "8 + 41.6 = 49.6 > 46: stopped";
    // A body exactly up to the base passes (half-open spans); one raw unit more is stopped.
    const Fixed exact = Fixed::from_raw(slab.base.raw - kRideHeight.raw);
    EXPECT_FALSE(volume_stops(slab, Fixed::zero(), exact));
    EXPECT_TRUE(volume_stops(slab, Fixed::zero(), Fixed::from_raw(exact.raw + 1)));
    // And the kerb stops both: its base is the ground.
    const Volume kerb{SolidMask{1, 1, 1}, Fixed::zero(), Fixed::from_int(40)};
    EXPECT_TRUE(volume_stops(kerb, Fixed::zero(), bike));
    EXPECT_TRUE(volume_stops(kerb, Fixed::zero(), tank));
}

} // namespace
} // namespace protekt::sim
