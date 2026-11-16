#include "protekt/game/pick.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

/// ROUND 19, L4: A BUILDING STOPS A BOLT -- ITS FOOTPRINT IS ITS HIT SHAPE. The owner, 2026-11-13:
///   "I was hiding behind the enemy recycler.  The alien guardian was still able to shoot at me,
///    right through the recycler.  The bolts should have been interrupted by the recycler."
/// and, on receiving the fix:
///   "I should still be able to lob a mortar over a recycler to hit the target behind it,
///    depending on the location."
/// So a FLAT bolt is stopped by any building (either side's) it enters, at the edge it came in
/// by; a LOBBED shell is stopped only where its arc is below the building's top, so it clears a
/// building from afar and is stopped by one it comes down on, or flies low past. The probe the
/// guns use to hold fire runs the same flight, so it sees the same things.
///
/// The geometry is flown by hand (`launch`, `advance_projectiles`) on a patch of open ground, the
/// units' heights being whatever the ground there is: `launch` aims at the target's real height.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;

struct Level {
    int x{};
    int y{};
};

/// The first spot with a crater-free patch of ground 21 tiles east and 11 tiles across, no part of
/// it more than 30 height units from its first tile: room for a gun, a building and a target.
[[nodiscard]] Level find_open() {
    for (int y = 40; y < kMapTiles - 40; ++y) {
        for (int x = 40; x < kMapTiles - 60; ++x) {
            const int h0 = world::sample(kSeed, x, y).height;
            bool level = true;
            for (int dy = -5; dy <= 5 && level; ++dy) {
                for (int dx = 0; dx <= 20 && level; ++dx) {
                    const world::Tile t = world::sample(kSeed, x + dx, y + dy);
                    level = t.terrain != world::Terrain::Crater && std::abs(t.height - h0) <= 30;
                }
            }
            if (level) {
                return Level{x, y};
            }
        }
    }
    return Level{-1, -1};
}

/// A unit `dx` / `dy` RAW tiles from the patch's first tile.
[[nodiscard]] Unit unit_at(std::uint32_t id, UnitKind kind, Side side, Level at, int dx_raw,
                           int dy_raw = 0) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = Fixed::from_raw(at.x * kTile + dx_raw);
    unit.y = Fixed::from_raw(at.y * kTile + dy_raw);
    unit.hp = spec_of(kind).hp;
    return unit;
}

struct Flown {
    std::vector<Unit> units;
    std::vector<Landing> landings;
};

/// Fires `shooter`'s shot at `target` and flies it to its end among `units` (which must hold the
/// shooter and the target).
[[nodiscard]] Flown fire(std::vector<Unit> units, std::size_t shooter, std::size_t target) {
    Projectile bolt = launch(units[shooter], units[target],
                             units[shooter].kind == UnitKind::MortarBike ? 16 : 10, kSeed);
    bolt.learns = false;
    std::vector<Projectile> flight{bolt};
    Flown flown{units, {}};
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 400 && !flight.empty(); ++tick) {
        advance_projectiles(flight, flown.units, impacts, flown.landings, kSeed);
    }
    return flown;
}

[[nodiscard]] bool hurt(const Flown& flown, std::size_t index) {
    return flown.units[index].hp < spec_of(flown.units[index].kind).hp;
}

// --- HIS CASE ------------------------------------------------------------------------------------

TEST(BuildingCover, HisCaseAGuardianBehindItsOwnRecyclerIsStoppedByIt) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // The Guardian (an alien turret) is 4 tiles behind the enemy's own deployed recycler (8 x 8,
    // centred 8 tiles out: it covers tiles 4..11); the player's tank hides 15 tiles out, beyond it.
    // The gun and the tank are on a line two and a half tiles off the recycler's centre: inside its
    // footprint (which reaches four tiles each way) but far outside the point-sized body it had.
    constexpr int kLane = -5 * kTile / 2;
    const Flown flown = fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 0, kLane),
                              unit_at(2, UnitKind::DeployedRecycler, Side::Enemy, at, 8 * kTile),
                              unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile, kLane)},
                             0, 2);
    EXPECT_FALSE(hurt(flown, 2))
        << "the bolt went through the recycler and hurt the tank behind it";
    EXPECT_TRUE(hurt(flown, 1)) << "the recycler it hit should have taken the bolt";
    ASSERT_EQ(flown.landings.size(), 1U);
    EXPECT_TRUE(flown.landings[0].hit);
}

TEST(BuildingCover, TheBoltStopsAtTheEdgeItEnteredWestAndEast) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // From the west: the footprint is tiles 4..11, so the bolt stops in tile 4 on the lane's row.
    constexpr int kLane = -5 * kTile / 2; // off the centre: inside the footprint, outside a point
    const Flown west = fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 0, kLane),
                             unit_at(2, UnitKind::DeployedRecycler, Side::Player, at, 8 * kTile),
                             unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile, kLane)},
                            0, 2);
    ASSERT_EQ(west.landings.size(), 1U);
    EXPECT_EQ(west.landings[0].tile_x, at.x + 4);
    EXPECT_EQ(west.landings[0].tile_y, at.y - 3);
    // From the east, the other way: footprint tiles 8..15, so it stops in tile 15.
    const Flown east = fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 20 * kTile, kLane),
                             unit_at(2, UnitKind::DeployedRecycler, Side::Player, at, 12 * kTile),
                             unit_at(3, UnitKind::Tank, Side::Player, at, 4 * kTile, kLane)},
                            0, 2);
    ASSERT_EQ(east.landings.size(), 1U);
    EXPECT_EQ(east.landings[0].tile_x, at.x + 15);
    EXPECT_EQ(east.landings[0].tile_y, at.y - 3);
}

// --- the boundary, per axis ----------------------------------------------------------------------

// The Training Centre is the 8 x 4 rectangle (x by y); turned, 4 x 8. Its centre is at (9, 0), so
// unturned it covers x 5..12 and y -2..1 (the rows from -2 up to, not including, +2), turned
// x 7..10 and y -4..3.
[[nodiscard]] Flown lane(Level at, bool turned, int lane_y_raw) {
    Unit centre = unit_at(2, UnitKind::TrainingCentre, Side::Player, at, 9 * kTile);
    centre.turned = turned;
    return fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 0, lane_y_raw), centre,
                 unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile, lane_y_raw)},
                0, 2);
}

TEST(BuildingCover, TheShortSideOfARectangleStopsABoltJustInsideItAndLetsOneJustOutsidePass) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    const int in = -(kTile + kTile * 9 / 10);  // 1.9 tiles north of the centre line: inside (< 2)
    const int out = -(2 * kTile + kTile / 10); // 2.1: beside it
    const Flown inside = lane(at, false, in);
    EXPECT_FALSE(hurt(inside, 2));
    EXPECT_TRUE(hurt(inside, 1));
    const Flown beside = lane(at, false, out);
    EXPECT_TRUE(hurt(beside, 2)) << "a bolt beside the footprint should reach its target";
    EXPECT_FALSE(hurt(beside, 1));
}

TEST(BuildingCover, TheLongSideOfARectangleStopsABoltJustInsideItAndLetsOneJustOutsidePass) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    const int in = -(3 * kTile + kTile * 9 / 10); // turned, the rectangle is 8 tall: 3.9 is inside
    const int out = -(4 * kTile + kTile / 10);    // 4.1 is beside it
    const Flown inside = lane(at, true, in);
    EXPECT_FALSE(hurt(inside, 2));
    EXPECT_TRUE(hurt(inside, 1));
    const Flown beside = lane(at, true, out);
    EXPECT_TRUE(hurt(beside, 2));
    EXPECT_FALSE(hurt(beside, 1));
    // And the same 3.9 lane, UNTURNED (4 tall: 2 each way), is well beside it.
    EXPECT_TRUE(hurt(lane(at, false, in), 2));
}

/// The same Training Centre (8 wide, 4 tall, covering x 5..12 and y -2..1) crossed the OTHER way:
/// a bolt flying south, ten tiles, along the column `x_raw` raw tiles from the patch's first.
[[nodiscard]] Flown column(Level at, int x_raw) {
    return fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, x_raw, -5 * kTile),
                 unit_at(2, UnitKind::TrainingCentre, Side::Player, at, 9 * kTile),
                 unit_at(3, UnitKind::Tank, Side::Player, at, x_raw, 5 * kTile)},
                0, 2);
}

TEST(BuildingCover, BoltsAcrossTheWidthAreStoppedWithinBothSidesAndPassBesideThem) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // The footprint reaches from x = 5 to x = 13 (centre 9, four each way).
    for (const int inside : {5 * kTile + kTile / 10, 12 * kTile + kTile * 9 / 10}) {
        const Flown flown = column(at, inside);
        EXPECT_FALSE(hurt(flown, 2)) << "x = " << inside / double(kTile);
        EXPECT_TRUE(hurt(flown, 1)) << "x = " << inside / double(kTile);
    }
    for (const int beside : {4 * kTile + kTile * 9 / 10, 13 * kTile + kTile / 10}) {
        const Flown flown = column(at, beside);
        EXPECT_TRUE(hurt(flown, 2)) << "x = " << beside / double(kTile);
        EXPECT_FALSE(hurt(flown, 1)) << "x = " << beside / double(kTile);
    }
}

// --- what does not stop a bolt -------------------------------------------------------------------

TEST(BuildingCover, ADestroyedBuildingStopsNothing) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    Unit wreck = unit_at(2, UnitKind::DeployedRecycler, Side::Enemy, at, 8 * kTile);
    wreck.alive = false;
    wreck.hp = 0;
    const Flown flown = fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 0), wreck,
                              unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile)},
                             0, 2);
    EXPECT_TRUE(hurt(flown, 2)) << "the tiles are free: the bolt reaches the tank";
    EXPECT_EQ(flown.units[1].hp, 0);
}

TEST(BuildingCover, ABuildingsOwnBoltLeavesItsOwnWalls) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // A Gun Tower stands inside its own footprint; its bolt is not stopped by itself.
    const Flown flown = fire({unit_at(1, UnitKind::GunTower, Side::Enemy, at, 0),
                              unit_at(2, UnitKind::Tank, Side::Player, at, 12 * kTile)},
                             0, 1);
    EXPECT_TRUE(hurt(flown, 1));
    EXPECT_FALSE(hurt(flown, 0));
}

TEST(BuildingCover, ATankKeepsItsOwnHitShape) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // A bolt aimed a whole tile past a tank's centre misses it, as it always did: nothing about a
    // non-building's hit shape changed.
    Unit gun = unit_at(1, UnitKind::Turret, Side::Enemy, at, 0);
    Unit bystander = unit_at(2, UnitKind::Tank, Side::Player, at, 8 * kTile, kTile);
    Unit target = unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile);
    const Flown flown = fire({gun, bystander, target}, 0, 2);
    EXPECT_FALSE(hurt(flown, 1));
    EXPECT_TRUE(hurt(flown, 2));
}

// --- HIS SECOND CASE: a lob goes over, unless it comes down on the building ----------------------

TEST(BuildingCover, HisSecondCaseAMortarLobsOverARecyclerAndHitsTheTargetBehindIt) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    for (const Side own : {Side::Player, Side::Enemy}) {
        // The recycler covers tiles 3..10 (the enemy's, or the mortar's own); the shell's arc is
        // above its roof (about 230 units, since N2: the model's height, not a tank's) at both
        // walls, and it comes down on the tank 14 tiles out. (OLD pin, before N2: the recycler
        // centred 5 out, covering 1..8, with the tank 13 out -- true only while a building's roof
        // was a tank's 66 units.)
        const Side other = own == Side::Player ? Side::Enemy : Side::Player;
        for (const Side building : {own, other}) {
            const Flown flown
                = fire({unit_at(1, UnitKind::MortarBike, own, at, 0),
                        unit_at(2, UnitKind::DeployedRecycler, building, at, 7 * kTile),
                        unit_at(3, UnitKind::Tank, other, at, 14 * kTile)},
                       0, 2);
            EXPECT_TRUE(hurt(flown, 2)) << "the lob was stopped by a building it passes over";
            EXPECT_FALSE(hurt(flown, 1)) << "the recycler under the arc was struck";
            ASSERT_EQ(flown.landings.size(), 1U);
            EXPECT_TRUE(flown.landings[0].lob);
        }
    }
}

TEST(BuildingCover, ALobThatComesDownOnABuildingIsStoppedByTheBuildingItLandsOn) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // Aimed at the recycler itself (centred 10 out, covering 6..13): the shell is high where it
    // crosses the near edge and falls onto the roof inside.
    const Flown flown = fire({unit_at(1, UnitKind::MortarBike, Side::Player, at, 0),
                              unit_at(2, UnitKind::DeployedRecycler, Side::Enemy, at, 10 * kTile)},
                             0, 1);
    EXPECT_TRUE(hurt(flown, 1));
    ASSERT_EQ(flown.landings.size(), 1U);
    EXPECT_TRUE(flown.landings[0].hit);
    EXPECT_GE(flown.landings[0].tile_x, at.x + 6);
    EXPECT_LE(flown.landings[0].tile_x, at.x + 13);
}

TEST(BuildingCover, ALobFlyingLowPastABuildingCloseByIsStoppedByItAndOneFromAfarClears) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // "Depending on the location": the same Power building (4 x 4) under the same gun. A short lob
    // is a flat one -- low where it crosses the building, so the building stops it; the long one
    // is high there and goes over. (OLD pin, before N2: the far shot's building was centred 3 out
    // with the tank at 13; the roof was a tank's 66 units, so any lob that high cleared.)
    const Flown near_shot
        = fire({unit_at(1, UnitKind::MortarBike, Side::Player, at, 0),
                unit_at(2, UnitKind::Power, Side::Enemy, at, 3 * kTile),
                unit_at(3, UnitKind::Tank, Side::Enemy, at, 6 * kTile + kTile / 2)},
               0, 2);
    EXPECT_TRUE(hurt(near_shot, 1)) << "the low lob crossed the building and was not stopped";
    EXPECT_FALSE(hurt(near_shot, 2));
    const Flown far_shot = fire({unit_at(1, UnitKind::MortarBike, Side::Player, at, 0),
                                 unit_at(2, UnitKind::Power, Side::Enemy, at, 7 * kTile),
                                 unit_at(3, UnitKind::Tank, Side::Enemy, at, 14 * kTile)},
                                0, 2);
    EXPECT_TRUE(hurt(far_shot, 2));
    EXPECT_FALSE(hurt(far_shot, 1));
}

TEST(BuildingCover, HisObservableAMortarClearsABuildingFromAfarAndIsStoppedByOneItComesDownOn) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // The recycler (centred 7 out) covers tiles 3..10 (up to x = 11). The shell falls about 120
    // height units a tile at the end, and the roof (the model's height, about 230 units since N2)
    // is well above the middle of the tank it is aimed at, so a target a tenth of a tile behind the
    // wall is one it comes down on the building to reach, and stopped there. One at the mortar's
    // full range, fourteen out, is crossed high and cleared. (OLD pin, before N2: the recycler
    // centred 9, covering 5..12, with the targets at 13.1 and 14, when the roof was 66 units.)
    const auto shot = [&](int target_raw) {
        return fire({unit_at(1, UnitKind::MortarBike, Side::Player, at, 0),
                     unit_at(2, UnitKind::DeployedRecycler, Side::Enemy, at, 7 * kTile),
                     unit_at(3, UnitKind::Tank, Side::Enemy, at, target_raw)},
                    0, 2);
    };
    const Flown close = shot(11 * kTile + kTile / 10);
    EXPECT_TRUE(hurt(close, 1)) << "it came down on the recycler";
    // (The burst is on the roof, not at the tank: whatever the splash does to the tank within
    // 1.5 tiles of it is the splash's, not the aim's.)
    ASSERT_EQ(close.landings.size(), 1U);
    EXPECT_GE(close.landings[0].tile_x, at.x + 3) << "it burst short of the building";
    EXPECT_LE(close.landings[0].tile_x, at.x + 10) << "it burst past the wall, not on the roof";
    const Flown far = shot(14 * kTile); // (the mortar's own range)
    EXPECT_TRUE(hurt(far, 2));
    EXPECT_FALSE(hurt(far, 1));
}

// --- the probe: the gun that would shoot through its own building holds fire -------------------

struct Skirmish {
    Sim sim;
    bool ever_fired{};
};

/// An enemy Tank at the patch's first tile, `with_building` an enemy deployed recycler (its
/// north-west tile is 2 out and 2 up: it covers 2..9 by -2..5, off the lane's own centre) in the
/// lane, and the player's (unarmed, wheeled) recycler 11 tiles out.
[[nodiscard]] Skirmish skirmish(Level at, bool with_building, Side building_side) {
    Scenario scenario{};
    scenario.seed = kSeed;
    const auto add = [&](Side side, UnitKind kind, int x, int y) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = x;
        spawn.tile_y = y;
        scenario.spawns.push_back(spawn);
    };
    add(building_side == Side::Enemy ? Side::Enemy : Side::Player, UnitKind::Tank, at.x, at.y);
    if (with_building) {
        add(building_side, UnitKind::DeployedRecycler, at.x + 2, at.y - 2);
    }
    const Side target_side = building_side == Side::Enemy ? Side::Player : Side::Enemy;
    add(target_side, UnitKind::Recycler, at.x + 11, at.y);
    return Skirmish{Sim::create(scenario), false};
}

[[nodiscard]] Skirmish run_for(Skirmish s, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        s.sim.step();
        s.ever_fired = s.ever_fired || !s.sim.projectiles().empty();
    }
    return s;
}

TEST(BuildingCover, AGunHoldsFireWhenItsOwnBuildingIsInTheLane) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    for (const Side side : {Side::Enemy, Side::Player}) {
        const Skirmish held = run_for(skirmish(at, true, side), 40);
        const UnitId target{static_cast<std::uint32_t>(3)};
        EXPECT_FALSE(held.ever_fired) << "a bolt was fired into the gun's own building";
        EXPECT_EQ(held.sim.find(target)->hp, spec_of(UnitKind::Recycler).hp);
        EXPECT_EQ(held.sim.find(UnitId{2})->hp, spec_of(UnitKind::DeployedRecycler).hp);
        // The control: the same field with no building in the lane fires and hurts the target.
        const Skirmish open = run_for(skirmish(at, false, side), 40);
        EXPECT_TRUE(open.ever_fired);
        EXPECT_LT(open.sim.find(UnitId{2})->hp, spec_of(UnitKind::Recycler).hp);
    }
}

// --- N2: THE LOB RULE -- THE ARC DECIDES ---------------------------------------------------------
// His ruling (2026-11-13, chosen from a card): "The arc decides: a shell that clears the roof lands
// behind it."  A building's roof is its model's height -- the scale times the authored body -- not
// the height of a tank; a shell is stopped by the footprint only if its own height where it
// crosses is below that roof. A mortar lob from the origin to a target 14 tiles out is, over open
// ground, about 140 height units high one tile out, 250 at three, 340 at six, and falls past 240
// again near eleven; a building's roof (a 4 x 4 power plant's, or the recycler's) is about 230.

/// The roof of a scaled building, in height units above the ground it stands on -- the model's
/// height (see `unit_box_height_metres`), built here from the constants the sim already has.
[[nodiscard]] std::int64_t roof_of(UnitKind kind) {
    const Fixed body = kind == UnitKind::Factory ? kFactoryBodyHeight : kBodyHeight;
    return kRideHeight.raw + body.raw * kBuildingFootprintScale;
}

/// A shooter, a building and a tank 14 tiles out (the mortar's own range), the building centred
/// `centre` tiles out on the line; the shot flown to its end.
[[nodiscard]] Flown over(UnitKind shooter, UnitKind building, int centre_raw,
                         Side own = Side::Player, int target_raw = 14 * kTile) {
    const Level at = find_open();
    const Side other = own == Side::Player ? Side::Enemy : Side::Player;
    return fire({unit_at(1, shooter, own, at, 0), unit_at(2, building, other, at, centre_raw),
                 unit_at(3, UnitKind::Tank, other, at, target_raw)},
                0, 2);
}

TEST(LobRule, TheSameShotAsABoltIsStoppedByALowBuildingAndAsALobLandsBehindIt) {
    ASSERT_GE(find_open().x, 0);
    // THE PAIR: one gun spot, one Power building (4 x 4, covering 5..9 of the line), one tank 14
    // tiles out. A bolt is stopped by the footprint at the edge it enters; the lob is high above
    // the roof there (about 320 units to the roof's 230) and lands on the tank behind.
    const Flown bolt = over(UnitKind::Turret, UnitKind::Power, 7 * kTile);
    EXPECT_TRUE(hurt(bolt, 1)) << "the bolt should have been stopped by the building";
    EXPECT_FALSE(hurt(bolt, 2));
    const Flown lob = over(UnitKind::MortarBike, UnitKind::Power, 7 * kTile);
    EXPECT_TRUE(hurt(lob, 2)) << "the lob should have cleared the roof and landed behind";
    EXPECT_FALSE(hurt(lob, 1));
    ASSERT_EQ(lob.landings.size(), 1U);
    EXPECT_TRUE(lob.landings[0].lob);
    EXPECT_TRUE(lob.landings[0].hit);
}

TEST(LobRule, HisCaseAMortarLobsOverARecyclerAndHitsTheTargetBehindIt) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // The deployed recycler (8 x 8) centred 7 out covers 3..11: the shell is above its roof at
    // both walls, so it goes over and comes down on the tank at the mortar's full range.
    for (const Side own : {Side::Player, Side::Enemy}) {
        const Flown lob = over(UnitKind::MortarBike, UnitKind::DeployedRecycler, 7 * kTile, own);
        EXPECT_TRUE(hurt(lob, 2));
        EXPECT_FALSE(hurt(lob, 1));
        // ... and a bolt from the same spot at the same tank is stopped by the same recycler.
        const Flown bolt = over(UnitKind::Turret, UnitKind::DeployedRecycler, 7 * kTile, own);
        EXPECT_TRUE(hurt(bolt, 1));
        EXPECT_FALSE(hurt(bolt, 2));
    }
}

TEST(LobRule, ABuildingTooTallForTheArcStopsALobAtTheEdgeItEntered) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // The Factory (8 x 8, modelled 18 m tall: a roof of about 580 units) centred 7 out
    // covers 3..11. The arc never gets near it (its peak is about 345), so the shell is stopped
    // where it crosses the near wall, in tile 3, exactly as a bolt would be.
    const Flown lob = over(UnitKind::MortarBike, UnitKind::Factory, 7 * kTile);
    EXPECT_TRUE(hurt(lob, 1));
    EXPECT_FALSE(hurt(lob, 2));
    ASSERT_EQ(lob.landings.size(), 1U);
    EXPECT_TRUE(lob.landings[0].hit);
    EXPECT_EQ(lob.landings[0].tile_x, at.x + 3);
}

TEST(LobRule, TheHeightAtTheCrossingDecidesNotTheTargetsPosition) {
    ASSERT_GE(find_open().x, 0);
    // The same gun, the same tank at 14: only the BUILDING moves along the line.
    //   centred 3 (covers 1..5): the shell is only ~150-250 units up as it crosses -> stopped.
    //   centred 7 (covers 5..9): ~320 up -> clears.
    //   centred 12 (covers 10..14): it is on the way down, under the roof by 11 -> stopped.
    const auto stopped = [&](int centre_tiles) {
        const Flown flown = over(UnitKind::MortarBike, UnitKind::Power, centre_tiles * kTile);
        EXPECT_NE(hurt(flown, 1), hurt(flown, 2)) << "centre " << centre_tiles;
        return hurt(flown, 1);
    };
    EXPECT_TRUE(stopped(3));
    EXPECT_FALSE(stopped(7));
    EXPECT_TRUE(stopped(12));
}

TEST(LobRule, AnApexAboveTheRoofDoesNotClearACrossingBelowIt) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // Fly the shot over nothing and take its highest point above the ground it started over.
    Unit gun = unit_at(1, UnitKind::MortarBike, Side::Player, at, 0);
    const Unit tank = unit_at(3, UnitKind::Tank, Side::Enemy, at, 14 * kTile);
    std::vector<Projectile> flight{launch(gun, tank, 16, kSeed)};
    flight[0].learns = false;
    std::vector<Unit> nothing;
    std::vector<Impact> impacts;
    std::vector<Landing> landings;
    std::int64_t apex = 0;
    const std::int64_t ground = ground_height(kSeed, gun.x, gun.y).raw;
    for (int tick = 0; tick < 400 && !flight.empty(); ++tick) {
        apex = std::max<std::int64_t>(apex, flight[0].z.raw - ground);
        advance_projectiles(flight, nothing, impacts, landings, kSeed);
    }
    ASSERT_GT(apex, roof_of(UnitKind::Power) + std::int64_t{50} * Fixed::kOne)
        << "the arc must peak clearly above the roof for this to mean anything";
    // A Power building centred 4 out (covering 2..6) is entered at about 200 units, under the
    // roof's 238, though the shell is above it from there on and climbs to some 340: stopped at the
    // wall.
    const Flown flown = over(UnitKind::MortarBike, UnitKind::Power, 4 * kTile);
    EXPECT_TRUE(hurt(flown, 1));
    EXPECT_FALSE(hurt(flown, 2));
}

TEST(LobRule, TheRoofIsTheHeightTheModelIsDrawnAndPickedAt) {
    // ONE roof height source: for every kind with a footprint the sim's roof is the same height the
    // game draws and picks the model at (`unit_box_height_metres`, 32 height units to the metre),
    // above the ride height; a kind without one has no roof but its body.
    for (int raw = 0; raw < 64; ++raw) {
        const auto kind = static_cast<UnitKind>(raw);
        if (!has_footprint(kind)) {
            EXPECT_EQ(roof_top(kind).raw, body_top(kind).raw);
            continue;
        }
        Unit unit{};
        unit.kind = kind;
        const double metres = game::unit_box_height_metres(unit);
        EXPECT_NEAR(static_cast<double>(roof_top(kind).raw - kRideHeight.raw),
                    metres * 32.0 * Fixed::kOne, 4.0)
            << "kind " << raw;
    }
}

/// A mortar on the patch's first tile, `building` (its north-west tile `nw_x` out and 2 up, so a
/// 4 x 4 one is on the lane) of its own side, and an enemy recycler `range` tiles out.
[[nodiscard]] Skirmish lob_skirmish(Level at, UnitKind building, int nw_x, int range) {
    Scenario scenario{};
    scenario.seed = kSeed;
    const auto add = [&](Side side, UnitKind kind, int x, int y) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = x;
        spawn.tile_y = y;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Enemy, UnitKind::MortarBike, at.x, at.y);
    add(Side::Enemy, building, at.x + nw_x, at.y - 2);
    add(Side::Player, UnitKind::Recycler, at.x + range, at.y);
    return Skirmish{Sim::create(scenario), false};
}

TEST(LobRule, TheProbeRefusesALobThatWouldHitItsOwnBuildingAndAllowsOneThatClearsIt) {
    const Level at = find_open();
    ASSERT_GE(at.x, 0);
    // Power covers nw..nw+4 of the lane. Near the gun (1..5) the lob is low there: it would come
    // down on the building, so the gun holds fire. Further out (5..9) it clears: the gun fires and
    // the recycler behind is hurt.
    const Skirmish held = run_for(lob_skirmish(at, UnitKind::Power, 1, 13), 200);
    EXPECT_FALSE(held.ever_fired) << "the mortar lobbed a shell into its own building";
    EXPECT_EQ(held.sim.find(UnitId{2})->hp, spec_of(UnitKind::Power).hp);
    const Skirmish clear = run_for(lob_skirmish(at, UnitKind::Power, 5, 13), 200);
    EXPECT_TRUE(clear.ever_fired) << "the mortar refused a lob that clears its own building";
    EXPECT_EQ(clear.sim.find(UnitId{2})->hp, spec_of(UnitKind::Power).hp);
    EXPECT_LT(clear.sim.find(UnitId{3})->hp, spec_of(UnitKind::Recycler).hp);
}

} // namespace
} // namespace protekt::sim
