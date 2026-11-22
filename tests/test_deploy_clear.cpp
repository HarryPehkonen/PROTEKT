#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/scrap.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// SLICE G2: A DEPLOYED BUILDING IS SOLID, AND ANYTHING THAT DEPLOYS SWEEPS ITS GROUND CLEAR.
/// The owner, 2026-11-22:
///   "After deploying the recycler, I realized there was scrap lying around underneath it.
///    Scavengers and myself seemed to be able to go through the deployed recycler, which is a bug,
///    but it enabled the scrap to be gathered. ... before a recycler deploys, all the scrap can be
///    swept out of the way so that none is on the ground that the recycler occupies once deployed."
/// and then: "So should a deployed turret."
///
/// So the rule is a DEPLOY rule: whatever stands on the ground a deployer is about to take has been
/// moved to the nearest tile a unit may enter OUTSIDE it, on every tick of the countdown. What the
/// rule covers is the Recycler (its 8 x 8 footprint) and the Turret (the one tile it stands on).
///
/// The rig is a patch of crater-free ground found by search (`find_open`), so the tests do not
/// depend on where a seed happens to be flat.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kPatch = 34; ///< the side of the clear square every rig lives in

struct Level {
    int x{};
    int y{};
};

/// The first kPatch x kPatch square of seed 42 with no crater in it.
[[nodiscard]] Level find_open() {
    for (int y = 40; y < kMapTiles - 40 - kPatch; ++y) {
        for (int x = 40; x < kMapTiles - 60 - kPatch; ++x) {
            bool clear = true;
            for (int dy = 0; dy < kPatch && clear; ++dy) {
                for (int dx = 0; dx < kPatch && clear; ++dx) {
                    clear = world::sample(kSeed, x + dx, y + dy).terrain != world::Terrain::Crater;
                }
            }
            if (clear) {
                return Level{x, y};
            }
        }
    }
    return Level{-1, -1};
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

[[nodiscard]] Scrap piece(int tx, int ty) {
    return Scrap{Fixed::from_raw(tx * Fixed::kOne + Fixed::kOne / 2),
                 Fixed::from_raw(ty * Fixed::kOne + Fixed::kOne / 2)};
}

[[nodiscard]] Sim rig(std::vector<Spawn> spawns, std::vector<Scrap> scrap = {}) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = std::move(spawns);
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 200));
    scenario.scrap = std::move(scrap);
    return Sim::create(scenario);
}

/// Runs the sim `ticks` ticks.
void run(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

[[nodiscard]] int tile_x_of(const Scrap& s) { return s.x.floor_int(); }
[[nodiscard]] int tile_y_of(const Scrap& s) { return s.y.floor_int(); }

/// How many pieces lie inside `site`.
[[nodiscard]] int buried(const Sim& sim, const Footprint& site) {
    int n = 0;
    for (const Scrap& s : sim.scraps()) {
        n += site.covers(tile_x_of(s), tile_y_of(s)) ? 1 : 0;
    }
    return n;
}

/// The recycler rig: a Recycler standing at the centre of the site `at` (north-west tile) with its
/// Deploy order already given. It is unit 0.
[[nodiscard]] Sim recycler_rig(Level at, std::vector<Scrap> scrap, std::vector<Spawn> more = {}) {
    std::vector<Spawn> spawns{spawn(UnitKind::Recycler, Side::Player, at.x + 4, at.y + 4)};
    spawns.insert(spawns.end(), more.begin(), more.end());
    Sim sim = rig(std::move(spawns), std::move(scrap));
    sim.enqueue(Order{sim.units().at(0).id, orders::Deploy{at.x, at.y}});
    return sim;
}

// --- PART ONE: A DEPLOYED RECYCLER STOPS UNITS

TEST(DeployClear, TheGroundIsOpen) { ASSERT_GE(find_open().x, 0); }

TEST(DeployClear, ADeployedRecyclerStopsUnitsOnEveryTileOfItsDrawnBox) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    Sim sim = recycler_rig(site, {});
    run(sim, static_cast<int>(kDeployTicks) + 60);
    const Unit& rec = sim.units().at(0);
    ASSERT_EQ(rec.kind, UnitKind::DeployedRecycler);
    // The mover's own answer, read on every tile of the box (the footprint's 8 x 8 less its outer
    // ring on each side: 6 x 6).
    const Footprint box = drawn_box(rec);
    ASSERT_EQ(box.width, 6);
    int stopped = 0;
    for (int ty = box.y; ty < box.y + box.height; ++ty) {
        for (int tx = box.x; tx < box.x + box.width; ++tx) {
            stopped += building_stops_units(sim.units(), tx, ty) ? 1 : 0;
        }
    }
    EXPECT_EQ(stopped, 36) << "a deployed recycler must be solid on its whole drawn box";
}

TEST(DeployClear, ATankCannotDriveThroughADeployedRecycler) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    // The tank starts west of the site on the site's middle row and is sent straight east.
    const int row = site.y + 4;
    Sim sim = recycler_rig(site, {}, {spawn(UnitKind::Tank, Side::Player, site.x - 4, row)});
    run(sim, static_cast<int>(kDeployTicks) + 60);
    ASSERT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler);
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(site.x + 12), Fixed::from_int(row)}});
    const Footprint box = drawn_box(sim.units().at(0));
    int inside = 0;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        const Unit& u = *sim.find(tank);
        inside += box.covers(u.x.floor_int(), u.y.floor_int()) ? 1 : 0;
    }
    EXPECT_EQ(inside, 0) << "the tank drove through the recycler's body";
    EXPECT_GE(sim.find(tank)->x.floor_int(), site.x + 8) << "it must get round to the far side";
}

TEST(DeployClear, ARecyclerWhoseCountdownIsRunningIsStillAUnitAndThenSolid) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    Sim sim = recycler_rig(site, {});
    run(sim, 10);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::Recycler);
    EXPECT_FALSE(building_stops_units(sim.units(), site.x + 4, site.y + 4));
    run(sim, static_cast<int>(kDeployTicks) + 20);
    EXPECT_TRUE(building_stops_units(sim.units(), site.x + 4, site.y + 4));
}

// --- THE DELIBERATE EXCEPTIONS SURVIVE: the platforms

TEST(DeployClear, TheServiceBayAndTheDowerAreStillDrivable) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level bay{open.x + 4, open.y + 12};
    const Level dower{open.x + 20, open.y + 12};
    Sim sim = rig({spawn(UnitKind::ServiceBay, Side::Player, bay.x, bay.y),
                   spawn(UnitKind::Dower, Side::Enemy, dower.x, dower.y)});
    int stops = 0;
    for (const Level at : {bay, dower}) {
        for (int ty = at.y; ty < at.y + 8; ++ty) {
            for (int tx = at.x; tx < at.x + 8; ++tx) {
                stops += building_stops_units(sim.units(), tx, ty) ? 1 : 0;
            }
        }
    }
    EXPECT_EQ(stops, 0) << "a platform is driven onto: no tile of it stops a unit";
    // ... and every other kind that has a footprint blocks: the rule is a LIST, not "none".
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        if (has_footprint(kind)) {
            const bool platform = kind == UnitKind::ServiceBay || kind == UnitKind::Dower;
            EXPECT_EQ(has_drivable_footprint(kind), platform) << "kind " << k;
        }
    }
    EXPECT_FALSE(has_drivable_footprint(UnitKind::DeployedRecycler));
    EXPECT_FALSE(has_drivable_footprint(UnitKind::Factory));
}

// --- PART TWO: THE GROUND IS SWEPT CLEAR BEFORE ANYTHING DEPLOYS

TEST(DeployClear, LitterInsideARecyclersFootprintIsMovedOutBeforeItDeploys) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    const Footprint footprint = footprint_at(site.x, site.y);
    std::vector<Scrap> litter;
    litter.reserve(8);
    for (int i = 0; i < 6; ++i) {
        litter.push_back(piece(site.x + 1 + i, site.y + 3));
    }
    litter.push_back(piece(site.x, site.y));         // a corner
    litter.push_back(piece(site.x + 7, site.y + 7)); // the opposite one
    Sim sim = recycler_rig(site, litter);
    ASSERT_EQ(buried(sim, footprint), 8) << "the rig must open with all of it inside";
    const std::size_t pieces = sim.scraps().size();
    // The countdown is 300 ticks, and the very last tick of it is the one that matters.
    run(sim, static_cast<int>(kDeployTicks) - 1);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::Recycler) << "still counting";
    EXPECT_EQ(buried(sim, footprint), 0) << "the ground is clear BEFORE the deploy completes";
    run(sim, 2);
    ASSERT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler);
    EXPECT_EQ(buried(sim, footprint), 0);
    EXPECT_EQ(sim.scraps().size(), pieces) << "swept, not destroyed: every piece is still there";
    // ... and each lies where a unit may enter, so it is reachable afterwards.
    for (const Scrap& s : sim.scraps()) {
        EXPECT_TRUE(tile_passable(kSeed, tile_x_of(s), tile_y_of(s)));
        EXPECT_FALSE(building_blocks(sim.units(), tile_x_of(s), tile_y_of(s)));
        EXPECT_FALSE(building_stops_units(sim.units(), tile_x_of(s), tile_y_of(s)));
    }
}

TEST(DeployClear, ASweptPieceLandsOnTheNearestFreeTileOutside) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    // One piece one tile in from the west edge: the nearest tile outside is on the column just west
    // of the footprint (Chebyshev distance two). The ring scan every settle uses takes the first
    // such tile in row order, which is the ring's top row: two rows up from the piece.
    Sim sim = recycler_rig(site, {piece(site.x + 1, site.y + 3)});
    run(sim, static_cast<int>(kDeployTicks) + 5);
    ASSERT_EQ(sim.scraps().size(), 1U);
    EXPECT_EQ(tile_x_of(sim.scraps().at(0)), site.x - 1);
    EXPECT_EQ(tile_y_of(sim.scraps().at(0)), site.y + 1);
}

TEST(DeployClear, LitterThatLandsOnTheSiteDuringTheCountdownIsSweptToo) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    const Footprint footprint = footprint_at(site.x, site.y);
    // A one-hit tank is shot to pieces on the site's east edge while the recycler counts: its wreck
    // lands on the site. The enemy (a tank's range is 12) is out of reach of the recycler itself.
    Spawn victim = spawn(UnitKind::Tank, Side::Player, site.x + 7, site.y + 3);
    victim.hp = 1;
    Sim sim = recycler_rig(site, {},
                           {victim, spawn(UnitKind::Tank, Side::Enemy, site.x + 18, site.y + 3)});
    int wrecked_at = -1;
    for (int t = 0; t < static_cast<int>(kDeployTicks) - 2; ++t) {
        sim.step();
        if (wrecked_at < 0 && !sim.scraps().empty()) {
            wrecked_at = t;
        }
    }
    ASSERT_GE(wrecked_at, 0) << "the rig must produce a wreck while the recycler is still counting";
    ASSERT_EQ(sim.units().at(0).kind, UnitKind::Recycler);
    ASSERT_TRUE(sim.units().at(0).alive);
    EXPECT_EQ(buried(sim, footprint), 0);
    run(sim, 10);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler);
    EXPECT_EQ(buried(sim, footprint), 0);
}

TEST(DeployClear, ALoneTurretSweepsTheTileItStandsOn) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const int tx = open.x + 16;
    const int ty = open.y + 16;
    Sim sim
        = rig({spawn(UnitKind::Turret, Side::Player, tx, ty)}, {piece(tx, ty), piece(tx + 1, ty)});
    sim.enqueue(Order{sim.units().at(0).id, orders::Deploy{tx, ty}});
    run(sim, static_cast<int>(kTurretDeployTicks) - 1);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::Turret) << "still counting";
    const Unit& turret = sim.units().at(0);
    const int ux = turret.x.floor_int();
    const int uy = turret.y.floor_int();
    for (const Scrap& s : sim.scraps()) {
        EXPECT_FALSE(tile_x_of(s) == ux && tile_y_of(s) == uy)
            << "a piece lies under the turret before it has deployed";
    }
    run(sim, 3);
    ASSERT_EQ(sim.units().at(0).kind, UnitKind::DeployedTurret);
    for (const Scrap& s : sim.scraps()) {
        EXPECT_FALSE(tile_x_of(s) == ux && tile_y_of(s) == uy);
    }
    EXPECT_EQ(sim.scraps().size(), 2U);
}

TEST(DeployClear, ALitterPileOnATurretsTileDoesNotBuryAPileBesideIt) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const int tx = open.x + 16;
    const int ty = open.y + 16;
    // A pile on a neighbouring tile is not under the turret and is not touched.
    const Scrap beside = piece(tx + 2, ty);
    Sim sim = rig({spawn(UnitKind::Turret, Side::Player, tx, ty)}, {beside});
    sim.enqueue(Order{sim.units().at(0).id, orders::Deploy{tx, ty}});
    run(sim, static_cast<int>(kTurretDeployTicks) + 5);
    ASSERT_EQ(sim.scraps().size(), 1U);
    EXPECT_EQ(sim.scraps().at(0).x.raw, beside.x.raw);
    EXPECT_EQ(sim.scraps().at(0).y.raw, beside.y.raw);
}

// --- NOWHERE TO GO: the piece is DESTROYED, and the deploy goes ahead

TEST(DeployClear, APieceWithNoLegalTileWithinReachIsDestroyedAndTheDeployGoesAhead) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    // Eight bomber bays (8 x 8) ring the 8 x 8 site exactly: every tile within eight of its edge is
    // another building's footprint, so there is nowhere a piece can be put.
    const Level site{open.x + 9, open.y + 9};
    std::vector<Spawn> ring;
    for (int j = 0; j < 3; ++j) {
        for (int i = 0; i < 3; ++i) {
            if (i != 1 || j != 1) {
                ring.push_back(spawn(UnitKind::BomberBay, Side::Player, open.x + 1 + (8 * i),
                                     open.y + 1 + (8 * j)));
            }
        }
    }
    const Footprint footprint = footprint_at(site.x, site.y);
    Sim sim
        = recycler_rig(site, {piece(site.x + 2, site.y + 2), piece(site.x + 5, site.y + 6)}, ring);
    ASSERT_EQ(sim.scraps().size(), 2U);
    run(sim, static_cast<int>(kDeployTicks) + 5);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler) << "the deploy is not refused";
    EXPECT_EQ(sim.scraps().size(), 0U) << "no tile to go to: destroyed, never buried";
    EXPECT_EQ(buried(sim, footprint), 0);
}

// --- THE CONTROL: a deploy on clear ground is as it was

TEST(DeployClear, ADeployOnClearGroundTakesExactlyTheCountdownAndMovesNoLitter) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site{open.x + 12, open.y + 12};
    // Litter OUTSIDE the footprint, near it: it must not move.
    const Scrap outside = piece(site.x - 2, site.y + 3);
    const Scrap far = piece(site.x + 15, site.y + 3);
    Sim sim = recycler_rig(site, {outside, far});
    run(sim, static_cast<int>(kDeployTicks) - 1);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::Recycler);
    run(sim, 1);
    EXPECT_EQ(sim.units().at(0).kind, UnitKind::DeployedRecycler)
        << "kDeployTicks (300) ticks from the order: the countdown is untouched";
    ASSERT_EQ(sim.scraps().size(), 2U);
    EXPECT_EQ(sim.scraps().at(0).x.raw, outside.x.raw);
    EXPECT_EQ(sim.scraps().at(0).y.raw, outside.y.raw);
    EXPECT_EQ(sim.scraps().at(1).x.raw, far.x.raw);
    EXPECT_EQ(sim.scraps().at(1).y.raw, far.y.raw);
}

} // namespace
} // namespace protekt::sim
