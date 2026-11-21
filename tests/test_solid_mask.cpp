#include "protekt/sim/orders.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

/// SLICE M4: THE SOLID MASK. The owner, asked whether impassable ground was worth keeping:
///   "Keep it in BOTH forms: world tiles (craters, valley walls, the map edge) and building-local
///    masks (the Gun Spire's prongs).  One concept, two frames."
/// N5 made a building's body its DRAWN BOX, one rectangle, and named this as its follow-up: "a true
/// silhouette declared, not implied". A building now declares a `SolidMask` in its OWN frame; the
/// mover (`building_stops_units`) and the bolt (`projectile.cpp`) both read `building_solid_at`, so
/// what may pass and what is hit are one shape.
///
/// The rig: a GUN SPIRE (4 x 4) with its north-west tile at (kX, kY) on the flat strip of seed 42.
/// Its mask is two PRONGS, the outer columns (kX and kX + 3), with a gap two wide between them
/// (kX + 1 and kX + 2) running the whole depth.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;
constexpr int kX = 60;
constexpr int kY = 26;

[[nodiscard]] bool open_ground() {
    for (int y = 22; y <= 35; ++y) {
        for (int x = 56; x <= 70; ++x) {
            if (world::sample(kSeed, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    return s;
}

[[nodiscard]] Sim rig(std::vector<Spawn> spawns) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = std::move(spawns);
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 200));
    return Sim::create(scenario);
}

[[nodiscard]] Unit unit_at(std::uint32_t id, UnitKind kind, Side side, int x_raw, int y_raw) {
    Unit unit{};
    unit.id = UnitId{id};
    unit.kind = kind;
    unit.side = side;
    unit.x = Fixed::from_raw(x_raw);
    unit.y = Fixed::from_raw(y_raw);
    unit.hp = spec_of(kind).hp;
    return unit;
}

/// A placed Gun Spire standing on the footprint kX .. kX + 3 by kY .. kY + 3 (centred on
/// (kX + 2, kY + 2)), turned or not.
[[nodiscard]] Unit spire(bool turned = false) {
    Unit unit = unit_at(1, UnitKind::GunSpire, Side::Player, (kX + 2) * kTile, (kY + 2) * kTile);
    unit.turned = turned;
    return unit;
}

[[nodiscard]] bool stops(const Unit& building, int tx, int ty) {
    const std::array<Unit, 1> one{building};
    return building_stops_units(one, tx, ty);
}

// --- THE CANONICAL CASE: BETWEEN THE PRONGS

TEST(SolidMask, TheGroundIsOpen) { ASSERT_TRUE(open_ground()); }

TEST(SolidMask, ATileBetweenTheProngsDoesNotStopAUnit) {
    const Unit unit = spire();
    for (int ty = kY; ty < kY + 4; ++ty) {
        for (const int tx : {kX + 1, kX + 2}) {
            EXPECT_FALSE(stops(unit, tx, ty)) << "gap tile " << tx << "," << ty;
        }
    }
}

TEST(SolidMask, ATankDrivesBetweenTheProngs) {
    ASSERT_TRUE(open_ground());
    // North of the spire in the gap's column, ordered to the south side of it: the straight line is
    // through the gap, and round the outside it is far longer. It was a solid block before M4.
    Sim sim = rig({spawn(UnitKind::GunSpire, Side::Player, kX, kY),
                   spawn(UnitKind::Tank, Side::Player, kX + 2, kY - 4)});
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(kX + 2), Fixed::from_int(kY + 8)}});
    bool between = false;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        const Unit& u = *sim.find(tank);
        const int tx = u.x.floor_int();
        const int ty = u.y.floor_int();
        between = between || ((tx == kX + 1 || tx == kX + 2) && ty >= kY + 1 && ty <= kY + 2);
    }
    EXPECT_TRUE(between) << "it never stood between the prongs: the gap is still a wall";
    EXPECT_GE(sim.find(tank)->y.floor_int(), kY + 7) << "it must reach the far side";
}

// --- THE NEGATIVE CONTROL: NOT THROUGH THE PRONGS

TEST(SolidMask, ATileInAProngStopsAUnit) {
    const Unit unit = spire();
    for (int ty = kY; ty < kY + 4; ++ty) {
        for (const int tx : {kX, kX + 3}) {
            EXPECT_TRUE(stops(unit, tx, ty)) << "prong tile " << tx << "," << ty;
        }
    }
    // And nothing outside the footprint is ever solid.
    EXPECT_FALSE(stops(unit, kX - 1, kY + 1));
    EXPECT_FALSE(stops(unit, kX + 4, kY + 1));
}

TEST(SolidMask, ATankIsRefusedTheProngsThemselves) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig({spawn(UnitKind::GunSpire, Side::Player, kX, kY),
                   spawn(UnitKind::Tank, Side::Player, kX - 4, kY + 1)});
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(kX + 2), Fixed::from_int(kY + 1)}});
    for (int t = 0; t < 600; ++t) {
        sim.step();
        const Unit& u = *sim.find(tank);
        const int tx = u.x.floor_int();
        const int ty = u.y.floor_int();
        const bool in_prong = (tx == kX || tx == kX + 3) && ty >= kY && ty < kY + 4;
        ASSERT_FALSE(in_prong) << "tick " << t << " at " << tx << "," << ty;
    }
}

// --- THE MASK TURNS WITH THE BUILDING

TEST(SolidMask, TheDrivableGapTurnsWithTheBuilding) {
    // Unturned the prongs are the west and east columns; turned a quarter they are the north and
    // south rows, and the gap is the two middle rows.
    const Unit upright = spire(false);
    const Unit turned = spire(true);
    EXPECT_TRUE(stops(upright, kX, kY + 1));
    EXPECT_FALSE(stops(turned, kX, kY + 1)) << "the west prong moved";
    EXPECT_FALSE(stops(upright, kX + 1, kY));
    EXPECT_TRUE(stops(turned, kX + 1, kY)) << "the gap moved: now it is a north prong";
    EXPECT_FALSE(stops(turned, kX + 1, kY + 1));
    EXPECT_FALSE(stops(turned, kX + 2, kY + 2));
    EXPECT_TRUE(stops(turned, kX + 2, kY + 3));
}

TEST(SolidMask, ATurnedMaskTurnsTheWayThePictureTurns) {
    // The Gun Spire is symmetric, so it cannot tell a quarter one way from the other. A mask that
    // is not: a 3 x 2 with one tile, the north-east corner. The picture turns a building so that
    // what faced south faces east (counter-clockwise from above), and the NE corner of the
    // rectangle goes to its NW; the turned rectangle is 2 wide and 3 tall.
    const SolidMask corner{3, 2, 0b000'100U}; // tile (2, 0)
    ASSERT_TRUE(corner.solid(2, 0));
    ASSERT_FALSE(corner.solid(0, 0));
    int solid_tiles = 0;
    for (int ty = 10; ty < 13; ++ty) {
        for (int tx = 20; tx < 22; ++tx) {
            const bool s = corner.solid_placed(20, 10, true, tx, ty);
            solid_tiles += s ? 1 : 0;
            EXPECT_EQ(s, tx == 20 && ty == 10) << tx << "," << ty;
        }
    }
    EXPECT_EQ(solid_tiles, 1);
    // Unturned it is the NE corner of a 3-wide rectangle.
    EXPECT_TRUE(corner.solid_placed(20, 10, false, 22, 10));
    EXPECT_FALSE(corner.solid_placed(20, 10, false, 20, 10));
}

// --- ONE GEOMETRY: WHAT MAY PASS IS WHAT A BOLT HITS

/// Flies an enemy Turret's bolt down the lane `lane_raw` at a Tank beyond `building`, to its end.
[[nodiscard]] bool bolt_is_stopped_by(const Unit& building, int lane_raw) {
    std::vector<Unit> units{building,
                            unit_at(3, UnitKind::Turret, Side::Enemy, lane_raw, (kY - 4) * kTile),
                            unit_at(4, UnitKind::Tank, Side::Player, lane_raw, (kY + 8) * kTile)};
    Projectile bolt = launch(units[1], units[2], 10, kSeed);
    bolt.learns = false;
    std::vector<Projectile> flight{bolt};
    std::vector<Landing> landings;
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 400 && !flight.empty(); ++tick) {
        advance_projectiles(flight, units, impacts, landings, kSeed);
    }
    const bool building_hurt = units[0].hp < spec_of(building.kind).hp;
    const bool tank_hurt = units[2].hp < spec_of(UnitKind::Tank).hp;
    EXPECT_NE(building_hurt, tank_hurt) << "the bolt must hit exactly one of the two";
    return building_hurt;
}

TEST(SolidMask, ABoltIsStoppedByExactlyWhatStopsAUnit) {
    for (const bool turned : {false, true}) {
        const Unit building = spire(turned);
        for (int tx = kX - 1; tx <= kX + 4; ++tx) {
            // A bolt down the middle of column `tx` crosses every row of it, so it is stopped by
            // the building exactly when some tile of that column stops a unit: one geometry, asked
            // of the mover's own predicate.
            bool column_solid = false;
            for (int ty = kY; ty < kY + 4; ++ty) {
                column_solid = column_solid || stops(building, tx, ty);
            }
            EXPECT_EQ(bolt_is_stopped_by(building, tx * kTile + kTile / 2), column_solid)
                << "column " << tx << (turned ? " turned" : "");
        }
    }
}

TEST(SolidMask, ABoltMayFlyBetweenTheProngsAndIsStoppedByAProng) {
    // The consequence stated: a bolt down the gap (x = kX + 2.0, the line between the two gap
    // columns) now passes the Spire and reaches what is behind it.
    EXPECT_FALSE(bolt_is_stopped_by(spire(), (kX + 2) * kTile))
        << "the gap is not a wall to a bolt";
    EXPECT_TRUE(bolt_is_stopped_by(spire(), kX * kTile + kTile / 2)) << "a prong is";
    EXPECT_TRUE(bolt_is_stopped_by(spire(), (kX + 3) * kTile + kTile / 2)) << "the other too";
}

TEST(SolidMask, TheSolidRectanglesAreExactlyTheSolidTiles) {
    for (const bool turned : {false, true}) {
        const Unit building = spire(turned);
        int from_rects = 0;
        for_each_solid_rect(building, [&](const Footprint& rect) {
            for (int ty = rect.y; ty < rect.y + rect.height; ++ty) {
                for (int tx = rect.x; tx < rect.x + rect.width; ++tx) {
                    EXPECT_TRUE(stops(building, tx, ty)) << tx << "," << ty;
                    ++from_rects;
                }
            }
        });
        int from_tiles = 0;
        for (int ty = kY - 2; ty < kY + 6; ++ty) {
            for (int tx = kX - 2; tx < kX + 6; ++tx) {
                from_tiles += stops(building, tx, ty) ? 1 : 0;
            }
        }
        EXPECT_EQ(from_rects, from_tiles);
        EXPECT_EQ(from_tiles, 8) << "two prongs of four tiles";
    }
}

// --- A BUILDING WITH NO MASK IS UNCHANGED

TEST(SolidMask, AKindWithNoMaskIsAsSolidAsItsDrawnBoxAsBefore) {
    for (int k = 0; k < kUnitKindCount; ++k) {
        const UnitKind kind = static_cast<UnitKind>(k);
        if (!has_footprint(kind) || !solid_mask_of(kind).empty()) {
            continue;
        }
        for (const bool turned : {false, true}) {
            Unit unit = unit_at(1, kind, Side::Player, 100 * kTile, 100 * kTile);
            unit.turned = turned;
            const Footprint box = drawn_box(unit);
            for (int ty = 90; ty < 110; ++ty) {
                for (int tx = 90; tx < 110; ++tx) {
                    EXPECT_EQ(building_solid_at(unit, tx, ty), box.covers(tx, ty))
                        << "kind " << k << " tile " << tx << "," << ty;
                }
            }
            int rects = 0;
            for_each_solid_rect(unit, [&](const Footprint& rect) {
                ++rects;
                EXPECT_EQ((std::array<int, 4>{rect.x, rect.y, rect.width, rect.height}),
                          (std::array<int, 4>{box.x, box.y, box.width, box.height}));
            });
            EXPECT_EQ(rects, 1) << "kind " << k;
        }
    }
}

TEST(SolidMask, OnlyTheGunSpireDeclaresAMaskAndItIsTheSizeOfItsFootprint) {
    for (int k = 0; k < kUnitKindCount; ++k) {
        const UnitKind kind = static_cast<UnitKind>(k);
        const SolidMask mask = solid_mask_of(kind);
        EXPECT_EQ(!mask.empty(), kind == UnitKind::GunSpire) << k;
        if (!mask.empty()) {
            EXPECT_EQ(mask.width, footprint_of(kind).width) << k;
            EXPECT_EQ(mask.height, footprint_of(kind).height) << k;
        }
    }
}

// --- PLACEMENT IS UNTOUCHED

TEST(SolidMask, TheGapIsDrivableAndStillNotBuildable) {
    ASSERT_TRUE(open_ground());
    Sim sim = rig({spawn(UnitKind::GunSpire, Side::Player, kX, kY)});
    for (const int tx : {kX, kX + 1, kX + 2, kX + 3}) {
        EXPECT_TRUE(building_blocks(sim.units(), tx, kY + 1)) << tx;
    }
    EXPECT_FALSE(place_ok(kSeed, sim.units(), UnitKind::Power, false, kX + 1, kY + 1));
    EXPECT_FALSE(deploy_site_ok(kSeed, sim.units(), kX + 1, kY + 1));
}

// --- THE OTHER FRAME: WORLD-ANCHORED SOLID TILES

TEST(SolidMask, AMaskPlacedAtAWorldTileIsTheSameConceptInTheWorldsFrame) {
    // The room author-placed impassable ground will use: a mask stood on a map tile, unturned. A
    // two-tile wall, east-west, at (30, 40).
    const SolidMask wall{2, 1, 0b11U};
    EXPECT_TRUE(wall.solid_placed(30, 40, false, 30, 40));
    EXPECT_TRUE(wall.solid_placed(30, 40, false, 31, 40));
    EXPECT_FALSE(wall.solid_placed(30, 40, false, 32, 40));
    EXPECT_FALSE(wall.solid_placed(30, 40, false, 30, 41));
    // And the Crater, the world's one impassable ground today, is still read by `tile_passable`.
    EXPECT_FALSE(tile_passable(kSeed, -1, 5));
    EXPECT_FALSE(tile_passable(kSeed, 5, kMapTiles));
    EXPECT_TRUE(tile_passable(kSeed, 60, 30));
}

} // namespace
} // namespace protekt::sim
