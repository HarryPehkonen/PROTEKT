#include "protekt/sim/orders.hpp"
#include "protekt/sim/projectile.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

/// SLICE G14: THE RECYCLER IS SOLID -- you cannot drive through it, and you cannot shoot through
/// it. The owner, after G2 had claimed it:
///   "Still a bug:  I can still drive through a recycler, and you can still shoot right through a
///    recycler."
///
/// WHAT WAS STILL OPEN. G2 made the deployed recycler solid on its DRAWN BOX: its 8 x 8 footprint
/// less a one-tile ring, 6 x 6. But the hull it is drawn as is not 6 x 6. It is the wheeled
/// recycler parked square on its site, 4.72 tiles across (x) and 10.4 tiles long (y), 10 m tall
/// (test_scene.cpp, RecyclerScale). Across, 6 covers 4.72. Along, it does not. The nose and the
/// tail, the footprint's first and last rows, were drawn hull and open ground. A tank went into
/// either end and slid along the box's face through the drawn plow or bumper. A bolt crossing an
/// end row flew through it. G2's test only drove at the recycler from the west, where the hull is
/// narrow, so it passed.
///
/// THE ONE FACT NOW: the deployed recycler's VOLUME (`volume_of`), whose extent is
/// `kDeployedRecyclerMask` (every row of the footprint, the six middle columns). The mover's
/// question (`building_stops_units`, so the drive, the strafe, the shove's carry, a jump's landing
/// and the route planner, which all ask `WorldGround::factor`) and the bolt's
/// (`for_each_solid_rect`) both read that volume. So every path is closed by that one edit, and
/// there is no second shape to forget. test_scene.cpp ties the drawing to it.
///
/// THE RIG: a recycler already deployed (spawned as the building: its spawn tile is its
/// footprint's north-west corner, S), on a crater-free patch. Its footprint is S..S+7 both ways.
/// The hull's plan inside it is columns S.x+1..S.x+6 on every row S.y..S.y+7. The NOSE ROW S.y is
/// where every test aims: drawn hull, and open ground before this slice.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kPatch = 34;
constexpr int kTile = Fixed::kOne;

struct Level {
    int x{};
    int y{};
};

/// The first kPatch x kPatch square of seed 42 with no crater in it (as test_deploy_clear.cpp).
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

/// The site: the recycler's footprint's north-west tile.
[[nodiscard]] Level site_of(Level open) { return Level{open.x + 12, open.y + 12}; }

/// The drawn hull's plan inside the footprint: the six middle columns, every row.
[[nodiscard]] Footprint hull_of(Level site) { return Footprint{site.x + 1, site.y, 6, 8}; }

/// A deployed recycler at `site` (unit 0), then `more`, then an enemy factory far off so that the
/// war is not over.
[[nodiscard]] Sim rig(Level site, std::vector<Spawn> more) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns.push_back(spawn(UnitKind::DeployedRecycler, Side::Player, site.x, site.y));
    scenario.spawns.insert(scenario.spawns.end(), more.begin(), more.end());
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 200));
    return Sim::create(scenario);
}

[[nodiscard]] bool in_hull(const Footprint& hull, const Unit& unit) {
    return hull.covers(unit.x.floor_int(), unit.y.floor_int());
}

TEST(RecyclerSolid, TheRigIsWhereItSays) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    const Sim sim = rig(site, {});
    const Unit& rec = sim.units().at(0);
    ASSERT_EQ(rec.kind, UnitKind::DeployedRecycler);
    const Footprint whole = footprint_around(rec);
    EXPECT_EQ(whole.x, site.x);
    EXPECT_EQ(whole.y, site.y);
    EXPECT_EQ(whole.width, 8);
    EXPECT_EQ(whole.height, 8);
}

// --- THE ONE FACT, read at the mover's question on every tile -----------------------------------

TEST(RecyclerSolid, TheMoverIsRefusedOnEveryTileOfTheDrawnHullAndNowhereElse) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    const Sim sim = rig(site, {});
    const Footprint hull = hull_of(site);
    int wrong = 0;
    for (int ty = site.y - 2; ty < site.y + 10; ++ty) {
        for (int tx = site.x - 2; tx < site.x + 10; ++tx) {
            const bool stops = building_stops_units(sim.units(), tx, ty);
            if (stops != hull.covers(tx, ty)) {
                ++wrong;
                ADD_FAILURE() << "tile " << tx - site.x << "," << ty - site.y << " (from the site)"
                              << (stops ? " stops a unit off the hull"
                                        : " is open inside the hull");
            }
        }
    }
    EXPECT_EQ(wrong, 0);
}

// --- PATH 1: THE DRIVEN MOVE (an order: MoveTo along the nose row) -------------------------------

TEST(RecyclerSolid, ATankSentAlongTheNoseRowNeverEntersTheHull) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    Sim sim = rig(site, {spawn(UnitKind::Tank, Side::Player, site.x - 4, site.y)});
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(site.x + 12), Fixed::from_int(site.y)}});
    const Footprint hull = hull_of(site);
    int inside = 0;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        inside += in_hull(hull, *sim.find(tank)) ? 1 : 0;
    }
    EXPECT_EQ(inside, 0) << "ticks the tank spent inside the drawn hull (its nose row)";
    EXPECT_GE(sim.find(tank)->x.floor_int(), site.x + 8) << "it must still get past";
}

// --- PATH 2: THE STRAFE (the helm: the player driving) -------------------------------------------

TEST(RecyclerSolid, ATankDrivenAtTheNoseByTheHelmStopsAtIt) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    // Due west of the nose row, driven straight east at it, as the player at the wheel does.
    Sim sim = rig(site, {spawn(UnitKind::Tank, Side::Player, site.x - 3, site.y)});
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::TakeControl{}});
    sim.step();
    const Footprint hull = hull_of(site);
    int inside = 0;
    bool at_the_face = false;
    for (int t = 0; t < 300; ++t) {
        sim.enqueue(Order{tank, orders::Strafe{Fixed::from_int(1), Fixed{}}});
        sim.step();
        const Unit& u = *sim.find(tank);
        inside += in_hull(hull, u) ? 1 : 0;
        at_the_face = at_the_face || (u.x.floor_int() == site.x && u.y.floor_int() == site.y);
    }
    EXPECT_EQ(inside, 0) << "ticks the driven tank spent inside the drawn hull";
    // It came right up to the hull, on the ring tile in front of the nose. What it does then is the
    // mover's slide (`plan_step`): it slides round a wall it cannot pass, as it does at any
    // building.
    EXPECT_TRUE(at_the_face) << "the rig must actually drive it up to the nose";
}

// --- PATH 3: THE SHOVE (a vehicle carried by a foe's push) ---------------------------------------

TEST(RecyclerSolid, AVehicleShovedAtTheNoseIsNotCarriedIntoIt) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    // A foe (an unarmed scavenger, so nobody dies) is first driven by the helm onto the ring tile
    // in front of the nose, a little into it (a unit is never SPAWNED inside a footprint), and left
    // there. Then a player tank is sent along the nose row through it and shoves it east, into the
    // hull's face.
    Sim sim = rig(site, {spawn(UnitKind::Tank, Side::Player, site.x - 5, site.y),
                         spawn(UnitKind::Scavenger, Side::Enemy, site.x - 1, site.y)});
    const UnitId pusher = sim.units().at(1).id;
    const UnitId foe = sim.units().at(2).id;
    sim.enqueue(Order{foe, orders::TakeControl{}});
    sim.step();
    const std::int32_t into_the_ring = (site.x * kTile) + (kTile / 4);
    for (int t = 0; t < 120 && sim.find(foe)->x.raw < into_the_ring; ++t) {
        sim.enqueue(Order{foe, orders::Strafe{Fixed::from_int(1), Fixed{}}});
        sim.step();
    }
    ASSERT_EQ(sim.find(foe)->x.floor_int(), site.x) << "the foe stands on the ring tile";
    ASSERT_EQ(sim.find(foe)->y.floor_int(), site.y);
    sim.enqueue(Order{pusher, orders::MoveTo{Fixed::from_int(site.x + 12), sim.find(foe)->y}});
    const Footprint hull = hull_of(site);
    int carried_in = 0;
    const std::int32_t foe_start = sim.find(foe)->x.raw;
    std::int32_t furthest = foe_start;
    for (int t = 0; t < 300; ++t) {
        sim.step();
        carried_in += in_hull(hull, *sim.find(foe)) ? 1 : 0;
        furthest = std::max(furthest, sim.find(foe)->x.raw);
    }
    EXPECT_GT(furthest - foe_start, 0) << "the rig must actually shove the foe east";
    EXPECT_EQ(carried_in, 0) << "ticks the shoved foe spent inside the drawn hull";
}

// --- PATH 4: THE ROUTE PLANNER -------------------------------------------------------------------

TEST(RecyclerSolid, ARoutePlannedPastTheNoseGoesRoundTheHull) {
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    Sim sim = rig(site, {spawn(UnitKind::Tank, Side::Player, site.x - 4, site.y)});
    const UnitId tank = sim.units().at(1).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(site.x + 12), Fixed::from_int(site.y)}});
    // The walk starts where the tank stands before it moves (`Route::from` is the current leg's).
    std::int64_t ax = sim.find(tank)->x.raw;
    std::int64_t ay = sim.find(tank)->y.raw;
    sim.step();
    const Route& route = sim.find(tank)->route;
    ASSERT_TRUE(route.has_goal);
    ASSERT_GT(route.count, 0) << "a plan was made";
    // Every leg, from where it starts to its waypoint, walked at an eighth of a tile: no point of
    // the planned path is on a tile of the hull.
    const Footprint hull = hull_of(site);
    int crossings = 0;
    for (std::uint8_t i = 0; i < route.count; ++i) {
        const std::int64_t bx
            = (static_cast<std::int64_t>(route.points.at(i).x) * kTile) + (kTile / 2);
        const std::int64_t by
            = (static_cast<std::int64_t>(route.points.at(i).y) * kTile) + (kTile / 2);
        for (int s = 0; s <= 64; ++s) {
            const std::int64_t px = ax + ((bx - ax) * s / 64);
            const std::int64_t py = ay + ((by - ay) * s / 64);
            crossings
                += hull.covers(static_cast<int>(px >> 16), static_cast<int>(py >> 16)) ? 1 : 0;
        }
        ax = bx;
        ay = by;
    }
    EXPECT_EQ(crossings, 0) << "the planned route runs through the drawn hull";
}

// --- PATH 5: THE BOLT ----------------------------------------------------------------------------

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

[[nodiscard]] Flown fire(std::vector<Unit> units, std::size_t shooter, std::size_t target) {
    Projectile bolt = launch(units[shooter], units[target], 10, kSeed);
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

/// A strip of level, crater-free ground 21 tiles east and 11 across (as test_building_cover.cpp).
[[nodiscard]] Level find_level() {
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

TEST(RecyclerSolid, ABoltAcrossTheNoseHitsTheRecyclerAndNotTheTankBehindIt) {
    const Level at = find_level();
    ASSERT_GE(at.x, 0);
    // The recycler is centred 8 tiles out on the strip's row, so its footprint is x 4..11 and
    // y -4..3 and the drawn hull runs from y -5.2 to +5.2. The lane is three and a half tiles north
    // of its centre: the footprint's first row, the nose, outside the old 6 x 6 box.
    constexpr int kLane = -7 * kTile / 2;
    const Flown flown = fire({unit_at(1, UnitKind::Turret, Side::Enemy, at, 0, kLane),
                              unit_at(2, UnitKind::DeployedRecycler, Side::Player, at, 8 * kTile),
                              unit_at(3, UnitKind::Tank, Side::Player, at, 15 * kTile, kLane)},
                             0, 2);
    EXPECT_FALSE(hurt(flown, 2)) << "the bolt flew through the recycler's nose into the tank";
    EXPECT_TRUE(hurt(flown, 1)) << "the recycler takes the bolt, as every building does";
    ASSERT_EQ(flown.landings.size(), 1U);
    EXPECT_TRUE(flown.landings[0].hit);
    EXPECT_EQ(flown.landings[0].tile_x, at.x + 5) << "at the hull's west face, column 1";
    EXPECT_EQ(flown.landings[0].tile_y, at.y - 4) << "on the nose row";
}

// --- THE DELIBERATE EXCEPTIONS SURVIVE
// ------------------------------------------------------------

TEST(RecyclerSolid, TheSideRingsAreStillOpenGround) {
    // Columns 0 and 7 of the footprint carry no hull, so they stay a ring a unit may drive on, as
    // N5 left every eight-wide building: two that touch still leave a corridor between them.
    const Level open = find_open();
    ASSERT_GE(open.x, 0);
    const Level site = site_of(open);
    const Sim sim = rig(site, {});
    for (int ty = site.y; ty < site.y + 8; ++ty) {
        EXPECT_FALSE(building_stops_units(sim.units(), site.x, ty)) << ty;
        EXPECT_FALSE(building_stops_units(sim.units(), site.x + 7, ty)) << ty;
    }
}

} // namespace
} // namespace protekt::sim
