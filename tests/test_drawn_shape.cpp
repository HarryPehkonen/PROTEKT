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
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

/// SLICE N5: WHAT MAY PASS IS THE BUILDING'S OWN SHAPE, NOT ITS FOOTPRINT SQUARE. The owner,
/// 2026-11-16:
///   "The Constructor ... seem[s] to get stuck behind buildings. ... when I see an opening between
///    two buildings, I can't seem to get through it. ... Maybe the SHAPE of the building should be
///    what determines if you can go through it.  Not just the footprint."
/// The shape is the DRAWN box (the very box `game::pick` picks): a building 8 tiles across is drawn
/// 6 across, so the tile ring around it is passable, and two 8-wide buildings that touch leave a
/// two-tile corridor between their drawn boxes. The SAME shape is the building's hit shape (a bolt
/// may fly down that corridor), while PLACEMENT stays the full square (the corridor is drivable
/// and not buildable).
///
/// The rig: two BOMBER BAYS (8 x 8) side by side, touching, on open ground: A's footprint covers
/// tiles kAx .. kAx + 7 (B's the next eight), both on rows kRowTop .. kRowTop + 7. Their drawn
/// boxes leave the outer tile of each free, so the tiles kAx + 7 and kBx (the ring) are a corridor
/// two wide, running north to south through the middle.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = Fixed::kOne;
/// The ground: the flat strip of seed 42 that `test_platform` stands on (x 56 .. 82, y 22 .. 35).
constexpr int kAx = 60; ///< building A's north-west tile (it covers kAx .. kAx + 7)
constexpr int kBx = 68; ///< B's: it touches A
constexpr int kRowTop = 26;
constexpr int kNorthY = 23; ///< the tank's start, north of both
constexpr int kSouthY = 35; ///< its goal, south of both

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

/// unit 0 = A, unit 1 = B, unit 2 = the vehicle `kind`, parked at (`x`, `y`).
[[nodiscard]] Sim two_buildings(UnitKind kind, int x, int y) {
    return rig({spawn(UnitKind::BomberBay, Side::Player, kAx, kRowTop),
                spawn(UnitKind::BomberBay, Side::Player, kBx, kRowTop),
                spawn(kind, Side::Player, x, y)});
}

[[nodiscard]] bool in_corridor(const Unit& unit) {
    const int tx = unit.x.floor_int();
    const int ty = unit.y.floor_int();
    return (tx == kAx + 7 || tx == kBx) && ty >= kRowTop + 1 && ty <= kRowTop + 6;
}

// --- HIS SECOND COMPLAINT: THE OPENING BETWEEN TWO BUILDINGS

TEST(DrawnShape, TheGroundIsOpen) { ASSERT_TRUE(open_ground()); }

TEST(DrawnShape, ATankDrivesThroughTheGapBetweenTwoTouchingBuildings) {
    ASSERT_TRUE(open_ground());
    Sim sim = two_buildings(UnitKind::Tank, kBx, kNorthY);
    const UnitId tank = sim.units().at(2).id;
    sim.enqueue(Order{tank, orders::MoveTo{Fixed::from_int(kBx), Fixed::from_int(kSouthY)}});
    bool through = false;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        through = through || in_corridor(*sim.find(tank));
    }
    EXPECT_TRUE(through) << "it went round the pair, or never got there: the gap is still a wall";
    EXPECT_GE(sim.find(tank)->y.floor_int(), kSouthY - 1) << "it must reach the far side";
}

// --- HIS FIRST: ROUND A BUILDING, WITHOUT LEAVING ITS NEIGHBOURHOOD

TEST(DrawnShape, AConstructorGoesAroundALoneBuildingThroughItsOwnRing) {
    ASSERT_TRUE(open_ground());
    // A alone (B is far to the east): the Constructor goes from A's west side to its east side. The
    // ring is tiles 60..67 of the footprint that the box (61..66) leaves free; the shortest way
    // round hugs it, so the Constructor stands on a footprint tile that is not drawn over.
    Sim sim = rig({spawn(UnitKind::BomberBay, Side::Player, kAx, kRowTop),
                   spawn(UnitKind::Constructor, Side::Player, kAx - 3, kRowTop + 4)});
    const UnitId id = sim.units().at(1).id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(kAx + 11), Fixed::from_int(kRowTop + 4)}});
    bool on_ring = false;
    int nearest_edge = 99;
    for (int t = 0; t < 900; ++t) {
        sim.step();
        const Unit& u = *sim.find(id);
        const bool inside_footprint
            = footprint_around(sim.units().at(0)).covers(u.x.floor_int(), u.y.floor_int());
        on_ring = on_ring || inside_footprint;
        nearest_edge = std::min(nearest_edge, std::abs(u.y.floor_int() - (kRowTop + 4)));
    }
    EXPECT_TRUE(on_ring) << "it never used the ring: it detoured round the whole square";
    EXPECT_GE(sim.find(id)->x.floor_int(), kAx + 10) << "it must reach the east side";
}

// --- ONE SHAPE FOR PASSING AND FOR HITTING

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

/// Flies a Turret's bolt at `target` among `units` to its end.
void fly(std::vector<Unit>& units, std::size_t shooter, std::size_t target) {
    Projectile bolt = launch(units[shooter], units[target], 10, kSeed);
    bolt.learns = false;
    std::vector<Projectile> flight{bolt};
    std::vector<Landing> landings;
    std::vector<Impact> impacts;
    for (int tick = 0; tick < 400 && !flight.empty(); ++tick) {
        advance_projectiles(flight, units, impacts, landings, kSeed);
    }
}

[[nodiscard]] bool hurt(const std::vector<Unit>& units, std::size_t index) {
    return units[index].hp < spec_of(units[index].kind).hp;
}

/// The pair, as units: A centred on x = 64, B on x = 72, both on y = 30. An enemy Turret fires from
/// the north at a Tank in the south, down the line `lane_raw`.
[[nodiscard]] std::vector<Unit> shoot_down(int lane_raw) {
    std::vector<Unit> units{unit_at(1, UnitKind::BomberBay, Side::Player, 64 * kTile, 30 * kTile),
                            unit_at(2, UnitKind::BomberBay, Side::Player, 72 * kTile, 30 * kTile),
                            unit_at(3, UnitKind::Turret, Side::Enemy, lane_raw, 23 * kTile),
                            unit_at(4, UnitKind::Tank, Side::Player, lane_raw, 35 * kTile)};
    fly(units, 2, 3);
    return units;
}

TEST(DrawnShape, ABoltDownTheCorridorPassesAndReachesTheTankBehind) {
    // A covers 60..67 (drawn box 61..66) and B 68..75 (box 69..74): the strip x = 67 .. 69, centred
    // on 68.0 -- the very line where the two footprints meet -- is the corridor.
    const std::vector<Unit> flown = shoot_down(68 * kTile);
    EXPECT_TRUE(hurt(flown, 3)) << "the corridor is not a wall to a bolt either";
    EXPECT_FALSE(hurt(flown, 0));
    EXPECT_FALSE(hurt(flown, 1));
}

TEST(DrawnShape, ABoltIntoTheBodyOfABuildingIsStopped) {
    const std::vector<Unit> a = shoot_down(64 * kTile + kTile / 2); // the middle of A's drawn box
    EXPECT_FALSE(hurt(a, 3));
    EXPECT_TRUE(hurt(a, 0));
    const std::vector<Unit> b = shoot_down(72 * kTile + kTile / 2); // the middle of B's
    EXPECT_FALSE(hurt(b, 3));
    EXPECT_TRUE(hurt(b, 1));
}

// --- PLACEMENT IS UNTOUCHED

TEST(DrawnShape, TheCorridorIsDrivableAndNotBuildable) {
    ASSERT_TRUE(open_ground());
    Sim sim = two_buildings(UnitKind::Tank, kBx, kNorthY);
    // A Power plant (4 x 4) aimed into the corridor would cover footprint tiles: refused, as it
    // always was, whichever way it is aimed.
    for (const int tx : {kAx + 4, kAx + 6, kAx + 7, kBx}) {
        EXPECT_FALSE(place_ok(kSeed, sim.units(), UnitKind::Power, false, tx, kRowTop + 2))
            << "tile " << tx;
    }
    EXPECT_FALSE(deploy_site_ok(kSeed, sim.units(), kAx + 6, kRowTop + 2));
    // And the ring tile itself is one the full-square rule still names as building ground.
    EXPECT_TRUE(building_blocks(sim.units(), kAx + 7, kRowTop + 2));
    // Open ground beside the pair is buildable, so the refusals above are the buildings' doing.
    EXPECT_TRUE(place_ok(kSeed, sim.units(), UnitKind::Power, false, kBx + 12, kRowTop + 2));
}

// --- THE SHAPE, AS A PER-KIND PROPERTY

[[nodiscard]] Unit placed(UnitKind kind, int centre_x, int centre_y, bool turned = false) {
    Unit unit = unit_at(1, kind, Side::Player, centre_x * kTile, centre_y * kTile);
    unit.turned = turned;
    return unit;
}

TEST(DrawnShape, AnEightWideBuildingIsDrawnSixWideAndAFourWideOneIsDrawnWhole) {
    // The drawn box is as far from the centre as the pick box reaches: `kBoxReachTiles` (three).
    const Footprint bay = drawn_box(placed(UnitKind::BomberBay, 64, 30));
    EXPECT_EQ((std::array<int, 4>{bay.x, bay.y, bay.width, bay.height}),
              (std::array<int, 4>{61, 27, 6, 6}));
    const Footprint power = drawn_box(placed(UnitKind::Power, 64, 30));
    EXPECT_EQ((std::array<int, 4>{power.x, power.y, power.width, power.height}),
              (std::array<int, 4>{62, 28, 4, 4}))
        << "a four-wide building is drawn to its whole footprint: nothing narrows";
    const Footprint hall = drawn_box(placed(UnitKind::TrainingCentre, 64, 30));
    EXPECT_EQ((std::array<int, 4>{hall.x, hall.y, hall.width, hall.height}),
              (std::array<int, 4>{61, 28, 6, 4}))
        << "8 x 4: narrowed along its long side only";
    const Footprint turned = drawn_box(placed(UnitKind::TrainingCentre, 64, 30, true));
    EXPECT_EQ((std::array<int, 4>{turned.x, turned.y, turned.width, turned.height}),
              (std::array<int, 4>{62, 27, 4, 6}))
        << "turned a quarter, the same box turns with it";
}

TEST(DrawnShape, EveryBoxLiesInsideItsFootprintAndOnlyTheEightWideSidesAreNarrowed) {
    for (int k = 0; k < kUnitKindCount; ++k) {
        const UnitKind kind = static_cast<UnitKind>(k);
        if (!has_footprint(kind)) {
            continue;
        }
        const Unit unit = placed(kind, 100, 100);
        const Footprint whole = footprint_around(unit);
        const Footprint box = drawn_box(unit);
        EXPECT_GE(box.x, whole.x) << k;
        EXPECT_GE(box.y, whole.y) << k;
        EXPECT_LE(box.x + box.width, whole.x + whole.width) << k;
        EXPECT_LE(box.y + box.height, whole.y + whole.height) << k;
        EXPECT_EQ(box.width, std::min(whole.width, 6)) << k;
        EXPECT_EQ(box.height, std::min(whole.height, 6)) << k;
    }
}

TEST(DrawnShape, DrivableIsAPassageAndTheOldFlagReadsIt) {
    for (int k = 0; k < kUnitKindCount; ++k) {
        const UnitKind kind = static_cast<UnitKind>(k);
        EXPECT_EQ(has_drivable_footprint(kind), passage_of(kind) == Passage::Drivable) << k;
    }
    EXPECT_EQ(passage_of(UnitKind::ServiceBay), Passage::Drivable);
    EXPECT_EQ(passage_of(UnitKind::Dower), Passage::Drivable);
    EXPECT_EQ(passage_of(UnitKind::Power), Passage::Blocks);
    EXPECT_EQ(passage_of(UnitKind::BomberBay), Passage::Blocks);
}

// --- WHAT STAYS

TEST(DrawnShape, AnOrdinaryUnitsHitBoxIsUnchanged) {
    // A Tank a bolt passes 0.3 tile beside is hit; one it passes 3 tiles beside is not.
    for (const auto& [off, expect] :
         {std::pair{kTile * 3 / 10, true}, std::pair{3 * kTile, false}}) {
        std::vector<Unit> units{
            unit_at(1, UnitKind::Turret, Side::Enemy, 58 * kTile, 30 * kTile),
            unit_at(2, UnitKind::Tank, Side::Player, 70 * kTile, 30 * kTile + off)};
        Unit aim = units[1];
        aim.y = Fixed::from_int(30); // aimed at the lane, not at the tank
        Projectile bolt = launch(units[0], aim, 10, kSeed);
        bolt.learns = false;
        std::vector<Projectile> flight{bolt};
        std::vector<Landing> landings;
        std::vector<Impact> impacts;
        for (int tick = 0; tick < 300 && !flight.empty(); ++tick) {
            advance_projectiles(flight, units, impacts, landings, kSeed);
        }
        EXPECT_EQ(hurt(units, 1), expect) << "offset " << off;
    }
}

TEST(DrawnShape, ABuildingsDrawnBodyStillRefusesAUnit) {
    ASSERT_TRUE(open_ground());
    Sim sim = two_buildings(UnitKind::Tank, kAx + 2, kNorthY);
    const UnitId tank = sim.units().at(2).id;
    sim.enqueue(
        Order{tank, orders::MoveTo{Fixed::from_int(kAx + 4), Fixed::from_int(kRowTop + 4)}});
    for (int t = 0; t < 600; ++t) {
        sim.step();
        const Unit& u = *sim.find(tank);
        const int tx = u.x.floor_int();
        const int ty = u.y.floor_int();
        const bool in_a = tx >= kAx + 1 && tx <= kAx + 6 && ty >= kRowTop + 1 && ty <= kRowTop + 6;
        ASSERT_FALSE(in_a) << "tick " << t << " at " << tx << "," << ty;
    }
}

TEST(DrawnShape, AServiceBayAndADowerAreStillDrivable) {
    ASSERT_TRUE(open_ground());
    for (const UnitKind kind : {UnitKind::ServiceBay, UnitKind::Dower}) {
        Sim sim = rig({spawn(kind, Side::Player, kBx, kRowTop),
                       spawn(UnitKind::Tank, Side::Player, kBx - 3, kRowTop + 4)});
        const UnitId tank = sim.units().at(1).id;
        sim.enqueue(
            Order{tank, orders::MoveTo{Fixed::from_int(kBx + 4), Fixed::from_int(kRowTop + 4)}});
        for (int t = 0; t < 400; ++t) {
            sim.step();
        }
        // In the DRAWN BOX, not merely on the ring: the ring is open to every building now, so
        // standing on it would not say the platform is drivable. (`test_platform` asks the
        // footprint, and so no longer tells a platform from an ordinary eight-wide building.)
        EXPECT_TRUE(drawn_box(sim.units().at(0))
                        .covers(sim.find(tank)->x.floor_int(), sim.find(tank)->y.floor_int()));
    }
}

} // namespace
} // namespace protekt::sim
