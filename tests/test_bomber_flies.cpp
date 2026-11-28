#include "protekt/app/map_file.hpp"
#include "protekt/game/player.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/sim/volume.hpp"
#include "protekt/world/ground.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <iostream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

/// SLICE G18: THE BOMBER FLIES -- an aircraft has an altitude, not a height. Harri: "And the Bomber
/// should fly over pretty much everything. I can't think of anything that could be blocking a
/// bomber."
///
/// G16 gave every kind a body height from its drawn model; the bomber's came out at 1100 mm, an
/// APC's, a ground vehicle's number on an aircraft. G18 gives an aircraft a FLIGHT ALTITUDE
/// (`flight_altitude_of`, 20 m above the ground under it) and keeps G16's ONE rule: a volume stops
/// a unit where their spans overlap, and an aircraft's span simply STARTS at its altitude
/// (`span_floor_of`). The map's heightless solid ground (the rockfall) is given a span too
/// (`kSolidGround`, the tallest roof's), so the one rule can be asked of it.
///
/// Every crossing here is DRIVEN, at the player's helm or by an order; the clearance table is the
/// rule's own arithmetic over every authored thing, printed.
namespace protekt::sim {
namespace {

constexpr int kTile = Fixed::kOne;
/// Height units to the metre (32).
constexpr double kUnitsPerMetre = static_cast<double>(kHeightUnitsPerTile) / kTileMetres;

[[nodiscard]] double units_of_raw(std::int64_t raw) {
    return static_cast<double>(raw) / Fixed::kOne;
}

[[nodiscard]] app::MapDoc proving_ground() {
    const app::MapParse parsed
        = app::load_map_file(std::string{PROTEKT_SOURCE_DIR} + "/assets/maps/proving_ground.map");
    EXPECT_TRUE(parsed.ok) << parsed.error;
    return parsed.map;
}

constexpr float kSouth = 0.0F;       ///< yaw 0 is +y, south (row 0 is the north edge)
constexpr float kWest = -1.5707963F; ///< -x

/// A unit at the player's helm on seed 0 (the installed map, when one is), a far-off enemy
/// factory, and the player's keys: test_slope_drive's rig, reduced to the track.
struct Helm {
    Sim sim;
    UnitId id{};
    game::PlayerCommander player;

    Helm(UnitKind kind, int tx, int ty, float heading, std::vector<PlacedVolume> volumes = {},
         const std::vector<Spawn>& extra = {})
        : sim(make(kind, tx, ty, std::move(volumes), extra)), id(sim.units().at(0).id) {
        sim.enqueue(Order{id, orders::TakeControl{}});
        player.set_heading(heading);
        game::InputState input{};
        input.throttle = 1.0F;
        player.set_input(input);
    }

    static Sim make(UnitKind kind, int tx, int ty, std::vector<PlacedVolume> volumes,
                    const std::vector<Spawn>& extra) {
        Scenario scenario{};
        scenario.seed = 0;
        scenario.volumes = std::move(volumes);
        Spawn unit{};
        unit.side = Side::Player;
        unit.kind = kind;
        unit.tile_x = tx;
        unit.tile_y = ty;
        scenario.spawns.push_back(unit);
        for (const Spawn& s : extra) {
            scenario.spawns.push_back(s);
        }
        Spawn base{};
        base.side = Side::Enemy;
        base.kind = UnitKind::Factory;
        base.tile_x = 56;
        base.tile_y = 56;
        scenario.spawns.push_back(base);
        return Sim::create(scenario);
    }

    /// Where the unit was after each of `ticks` ticks, in tiles.
    std::vector<std::array<double, 2>> drive(int ticks) {
        std::vector<std::array<double, 2>> track;
        for (int t = 0; t < ticks; ++t) {
            std::vector<Order> orders;
            player.think(id, observe(sim, Side::Player), orders);
            for (const Order& order : orders) {
                sim.enqueue(order);
            }
            sim.step();
            const Unit& u = *sim.find(id);
            track.push_back(
                {static_cast<double>(u.x.raw) / kTile, static_cast<double>(u.y.raw) / kTile});
        }
        return track;
    }
};

[[nodiscard]] std::string print(const std::vector<std::array<double, 2>>& track) {
    std::string out;
    for (std::size_t t = 0; t < track.size(); t += 5) {
        out += std::format("  tick {:3}  x {:7.3f}  y {:7.3f}\n", t + 1, track.at(t)[0],
                           track.at(t)[1]);
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

// --- 1. THE CLEARANCE TABLE ----------------------------------------------------------------------

TEST(BomberFlies, TheClearanceTableOverEveryAuthoredThingAndTheSmallestIsTheFinding) {
    // For every authored shape (each of the Proving Ground's terrain volumes, tile by tile, and its
    // solid ground) and every unit volume (each building kind's `volume_of`, which a building of
    // that kind on the Proving Ground stands as), the bomber's clearance: where its span starts,
    // less the thing's top. A positive clearance is passed over; the rule (`volume_stops`) is asked
    // too, so the table cannot say "clear" while the mover says "stopped".
    const Fixed floor = span_floor_of(UnitKind::Bomber);
    const Fixed body = body_height_of(UnitKind::Bomber);
    struct Row {
        std::string what;
        Volume volume;
    };
    std::vector<Row> rows;
    const app::MapDoc map = proving_ground();
    const std::vector<PlacedVolume> placed = app::placed_volumes(map);
    ASSERT_FALSE(placed.empty()) << "the Proving Ground has its kerb";
    rows.reserve(placed.size()); // one row per placed volume, known before the loop
    for (std::size_t i = 0; i < placed.size(); ++i) {
        rows.push_back(Row{
            std::format("map volume {} at ({}, {}) (the kerb)", i, placed.at(i).x, placed.at(i).y),
            placed.at(i).volume});
    }
    int solid_tiles = 0;
    for (const std::uint8_t s : map.solid) {
        solid_tiles += s != 0 ? 1 : 0;
    }
    ASSERT_GT(solid_tiles, 0) << "the Proving Ground has its rockfall";
    rows.push_back(
        Row{std::format("solid ground ({} tiles: the rockfall)", solid_tiles), kSolidGround});
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        if (has_footprint(kind)) {
            rows.push_back(Row{std::format("building kind {:2} ({})", k, spec_of(kind).name),
                               volume_of(kind)});
        }
    }
    std::cout << std::format("[ G18 ] the bomber's span starts {:.4f} units ({:.3f} m) above the "
                             "ground under it\n",
                             units_of_raw(floor.raw), units_of_raw(floor.raw) / kUnitsPerMetre);
    std::cout << "[ G18 ] thing                                           top (units)  top (m)  "
                 "clearance (units)  clearance (m)\n";
    double smallest = std::numeric_limits<double>::max();
    std::string smallest_what;
    for (const Row& r : rows) {
        const double top = units_of_raw(r.volume.top.raw);
        const double clear = units_of_raw(floor.raw) - top;
        std::cout << std::format("[ G18 ] {:46}  {:11.4f}  {:7.3f}  {:17.4f}  {:13.3f}\n", r.what,
                                 top, top / kUnitsPerMetre, clear, clear / kUnitsPerMetre);
        EXPECT_GT(clear, 0.0) << r.what;
        EXPECT_FALSE(volume_stops(r.volume, Fixed::zero(), body, floor)) << r.what;
        if (clear < smallest) {
            smallest = clear;
            smallest_what = r.what;
        }
    }
    std::cout << std::format("[ G18 ] SMALLEST CLEARANCE: {:.4f} units ({:.3f} m), over {}\n",
                             smallest, smallest / kUnitsPerMetre, smallest_what);
    // The finding: the tallest thing is the Factory's roof (and the solid ground, given it), 584;
    // the bomber clears it by 56 units, 1.75 m.
    EXPECT_EQ(volume_of(UnitKind::Factory).top.raw, 584 * Fixed::kOne);
    EXPECT_DOUBLE_EQ(smallest, 56.0);
}

// --- 2. THE TALL THING ---------------------------------------------------------------------------

/// Seed 42's flat strip (x 56 .. 82, y 22 .. 35), test_volume's and test_vehicle_height's rig: a
/// wall one tile deep and 27 wide on row 29, `top_raw` high from the ground, and a unit 1.5 tiles
/// north of it on column 69 ordered to the far side. Whether it came out on the far side without
/// leaving the wall's width (so a way round the end is not counted).
[[nodiscard]] bool crosses_wall(UnitKind kind, std::int32_t top_raw) {
    constexpr int kLeft = 56;
    constexpr int kWidth = 27;
    constexpr int kWallY = 29;
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns
        = {spawn(kind, Side::Player, 69, 27), spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    for (int x = kLeft; x < kLeft + kWidth; x += 8) {
        const int width = std::min(8, kLeft + kWidth - x);
        const SolidMask row{width, 1, (std::uint64_t{1} << static_cast<unsigned>(width)) - 1};
        scenario.volumes.push_back(
            PlacedVolume{x, kWallY, false, Volume{row, Fixed::zero(), Fixed::from_raw(top_raw)}});
    }
    Sim sim = Sim::create(scenario);
    const UnitId id = sim.units().at(0).id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(69), Fixed::from_int(34)}});
    bool inside = true;
    for (int t = 0; t < 90; ++t) {
        sim.step();
        const Unit& u = *sim.find(id);
        const double x = static_cast<double>(u.x.raw) / kTile;
        const double y = static_cast<double>(u.y.raw) / kTile;
        inside = inside && x > kLeft + 1.0 && x < kLeft + kWidth - 1.0;
        if (y > kWallY + 1.0) {
            return inside;
        }
    }
    return false;
}

TEST(BomberFlies, TheBomberCrossesTheTallestVolumeInTheGameAndATankDoesNot) {
    for (int y = 22; y <= 35; ++y) {
        for (int x = 56; x <= 82; ++x) {
            ASSERT_EQ(world::sample(42, x, y).terrain, world::Terrain::Flat) << x << ", " << y;
        }
    }
    // The tallest volume in the game is the Factory's: a terrain wall that tall, from the ground.
    std::int32_t tallest = 0;
    for (int k = 0; k < kUnitKindCount; ++k) {
        tallest = std::max(tallest, volume_of(static_cast<UnitKind>(k)).top.raw);
    }
    EXPECT_EQ(tallest, volume_of(UnitKind::Factory).top.raw);
    const bool bomber = crosses_wall(UnitKind::Bomber, tallest);
    const bool tank = crosses_wall(UnitKind::Tank, tallest);
    std::cout << std::format("[ G18 ] a wall {:.1f} units ({:.2f} m) high: bomber {}, tank {}\n",
                             units_of_raw(tallest), units_of_raw(tallest) / kUnitsPerMetre,
                             bomber ? "CROSSED" : "stopped", tank ? "CROSSED" : "stopped");
    EXPECT_TRUE(bomber) << "the bomber flies over the tallest thing in the game";
    EXPECT_FALSE(tank) << "a tank is stopped by it";
    // And the Factory itself, standing: every one of its solid tiles stops a tank and not a bomber.
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns = {spawn(UnitKind::Factory, Side::Enemy, 120, 120)};
    const Sim sim = Sim::create(scenario);
    const Unit& factory = sim.units().at(0);
    const Footprint area = footprint_around(factory);
    int stops_tank = 0;
    int stops_bomber = 0;
    for (int ty = area.y; ty < area.y + area.height; ++ty) {
        for (int tx = area.x; tx < area.x + area.width; ++tx) {
            stops_tank += building_stops_units(sim.units(), tx, ty, Fixed::zero(),
                                               body_height_of(UnitKind::Tank))
                              ? 1
                              : 0;
            stops_bomber += building_stops_units(sim.units(), tx, ty, Fixed::zero(),
                                                 body_height_of(UnitKind::Bomber),
                                                 span_floor_of(UnitKind::Bomber))
                                ? 1
                                : 0;
        }
    }
    std::cout << std::format("[ G18 ] a standing Factory: {} tiles stop a tank, {} a bomber\n",
                             stops_tank, stops_bomber);
    EXPECT_GT(stops_tank, 0) << "the Factory stops a tank";
    EXPECT_EQ(stops_bomber, 0) << "and no tile of it stops a bomber";
}

// --- HIS ACCEPTANCE: the kerb and the rockfall, at the helm
// ---------------------------------------

TEST(BomberFlies, InTheValleyTheKerbAndTheRockfallTurnATankAsideAndNeitherTurnsTheBomber) {
    // Column 31, south from row 16: the kerb (a volume 0..40 across x 30..35 of row 24) and then
    // the rockfall (solid ground at x 30..32, rows 32..33) lie straight ahead. The tank is turned
    // off the column; the bomber holds it past both.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    Helm tank(UnitKind::Tank, 31, 16, kSouth, app::placed_volumes(map));
    const auto tank_track = tank.drive(120);
    Helm bomber(UnitKind::Bomber, 31, 16, kSouth, app::placed_volumes(map));
    const auto bomber_track = bomber.drive(60);
    std::cout << "[ G18 ] tank at the helm, column 31 south:\n" << print(tank_track);
    std::cout << "[ G18 ] bomber at the helm, column 31 south:\n" << print(bomber_track);
    EXPECT_NE(tank.sim.find(tank.id)->x.floor_int(), 31) << "the kerb turns a tank aside";
    double furthest = 0.0;
    for (const auto& p : bomber_track) {
        if (p[1] < 35.0) {
            EXPECT_EQ(static_cast<int>(p[0]), 31) << "the bomber left its column at y " << p[1];
        }
        furthest = std::max(furthest, p[1]);
    }
    EXPECT_GT(furthest, 35.0) << "the bomber is past the kerb (row 24) and the rockfall (32..33)";
}

// --- 3. THE GROUND VEHICLES ARE UNCHANGED --------------------------------------------------------

/// test_vehicle_height's slab rig: a slab one tile deep, 20 thick, its underside `base_raw` up,
/// on seed 42's flat strip; whether `kind` ordered across it came out beneath it.
[[nodiscard]] bool passes_under(UnitKind kind, std::int32_t base_raw) {
    constexpr int kLeft = 56;
    constexpr int kWidth = 27;
    constexpr int kSlabY = 29;
    Scenario scenario{};
    scenario.seed = 42;
    scenario.spawns
        = {spawn(kind, Side::Player, 69, 27), spawn(UnitKind::Factory, Side::Enemy, 200, 200)};
    for (int x = kLeft; x < kLeft + kWidth; x += 8) {
        const int width = std::min(8, kLeft + kWidth - x);
        const SolidMask row{width, 1, (std::uint64_t{1} << static_cast<unsigned>(width)) - 1};
        scenario.volumes.push_back(PlacedVolume{
            x, kSlabY, false,
            Volume{row, Fixed::from_raw(base_raw), Fixed::from_raw(base_raw + (20 * kTile))}});
    }
    Sim sim = Sim::create(scenario);
    const UnitId id = sim.units().at(0).id;
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(69), Fixed::from_int(34)}});
    bool inside = true;
    for (int t = 0; t < 90; ++t) {
        sim.step();
        const Unit& u = *sim.find(id);
        const double x = static_cast<double>(u.x.raw) / kTile;
        inside = inside && x > kLeft + 1.0 && x < kLeft + kWidth - 1.0;
        if (static_cast<double>(u.y.raw) / kTile > kSlabY + 1.0) {
            return inside;
        }
    }
    return false;
}

[[nodiscard]] std::int32_t lowest_base_passed_under(UnitKind kind) {
    std::int32_t stopped = 0;
    std::int32_t passed = 128 * kTile;
    EXPECT_FALSE(passes_under(kind, stopped));
    EXPECT_TRUE(passes_under(kind, passed));
    while (passed - stopped > 1) {
        const std::int32_t mid = stopped + ((passed - stopped) / 2);
        (passes_under(kind, mid) ? passed : stopped) = mid;
    }
    return passed;
}

TEST(BomberFlies, TheGroundLadderIsReMeasuredAndIdenticalToG17) {
    // 479c0251's numbers: the bike 960 mm, the APC 1100, the tank 1300 -- 30.72, 35.2 and 41.6
    // units -- so the lowest slab base each drives under is its ride height (8) plus that, to the
    // raw unit of the bisection: 38.72, 43.2 and 49.6.
    struct Rung {
        UnitKind kind;
        const char* name;
        int mm;
        double lowest;
    };
    const std::array<Rung, 3> ladder{Rung{UnitKind::MortarBike, "mortar bike", 960, 38.72},
                                     Rung{UnitKind::Apc, "APC", 1100, 43.2},
                                     Rung{UnitKind::Tank, "tank", 1300, 49.6}};
    for (const Rung& r : ladder) {
        EXPECT_EQ(span_floor_of(r.kind).raw, kRideHeight.raw) << r.name << " is on the ground";
        EXPECT_EQ(body_height_of(r.kind).raw, height_of_mm(r.mm).raw) << r.name;
        const double lowest = units_of_raw(lowest_base_passed_under(r.kind));
        std::cout << std::format("[ G18 ] {:12} {:5} mm  lowest base passed under {:.5f} "
                                 "(G17: {:.5f})\n",
                                 r.name, r.mm, lowest, r.lowest);
        EXPECT_NEAR(lowest, r.lowest, 2.0 / Fixed::kOne) << r.name;
    }
}

TEST(BomberFlies, TheBomberIsTheOnlyAircraftAndEveryOtherKindStartsAtTheRideHeight) {
    std::vector<int> aircraft;
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        if (flies(kind)) {
            aircraft.push_back(k);
        } else {
            EXPECT_EQ(span_floor_of(kind).raw, kRideHeight.raw) << "kind " << k;
            EXPECT_EQ(flight_altitude_of(kind).raw, 0) << "kind " << k;
        }
    }
    EXPECT_EQ(aircraft, std::vector<int>{static_cast<int>(UnitKind::Bomber)});
    EXPECT_EQ(flight_altitude_of(UnitKind::Bomber).raw, height_of_mm(20000).raw) << "20 m";
}

// --- WHAT CAN STILL STOP A BOMBER: named, each with its drive ------------------------------------
//
// The height rule no longer stops it. These are the things that are NOT heights -- each a rule of
// its own that has no altitude in it -- and each still stops a bomber. Reported, not changed.

TEST(WhatStillStopsABomber, TheMapsEdge) {
    // Off the map every tile reads as Crater (`world::detail::ground_tile`), whose speed factor is
    // zero: the edge is a crater all the way round.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    Helm bomber(UnitKind::Bomber, 40, 58, kSouth);
    const auto track = bomber.drive(60);
    std::cout << "[ G18 ] bomber at the helm, south at the map's edge:\n" << print(track);
    for (const auto& p : track) {
        EXPECT_LT(p[1], 64.0) << "the bomber left the map";
    }
}

TEST(WhatStillStopsABomber, ACraterTile) {
    // The Proving Ground's crater (x 9..11, rows 44..46): terrain whose speed factor is zero. A
    // crater is not a volume; nothing about it has a height the one rule could ask.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    Helm bomber(UnitKind::Bomber, 10, 38, kSouth);
    const auto track = bomber.drive(60);
    std::cout << "[ G18 ] bomber at the helm, south at the crater:\n" << print(track);
    for (const auto& p : track) {
        EXPECT_LT(p[1], 44.0) << "the bomber entered the crater";
    }
}

TEST(WhatStillStopsABomber, TheDropDrivenBackUp) {
    // The drop (corner x 46 at 120, corner x 47 at 0, rows 20..29): a face too steep to climb is a
    // WALL to the slope rule (`drivable`), which reads the ground's heights and nothing about the
    // unit's. The bomber meets the face and is turned along its foot (x 46.09); it never reaches
    // the plateau, whose edge is corner 46.
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    Helm bomber(UnitKind::Bomber, 50, 25, kWest);
    const auto track = bomber.drive(60);
    std::cout << "[ G18 ] bomber at the helm, west at the foot of the drop:\n" << print(track);
    for (const auto& p : track) {
        EXPECT_GT(p[0], 46.0) << "the bomber climbed the drop";
    }
}

TEST(WhatStillStopsABomber, AVehicleOnTheGroundInItsPath) {
    // A body is not a volume: the mover's "is a vehicle in the way" (`vehicle_in_the_way`) is a
    // distance on the ground, with no height in it. A friendly tank standing on the column, on the
    // Proving Ground's flat south-west (x 14..25, rows 51..63, all level and open).
    const app::MapDoc map = proving_ground();
    const world::ScopedGround guard(app::map_ground(map));
    Helm bomber(UnitKind::Bomber, 18, 51, kSouth, {},
                {spawn(UnitKind::Tank, Side::Player, 18, 57)});
    UnitId tank_id{};
    for (const Unit& u : bomber.sim.units()) {
        if (u.kind == UnitKind::Tank) {
            tank_id = u.id;
        }
    }
    double closest = std::numeric_limits<double>::max();
    for (int t = 0; t < 40; ++t) {
        bomber.drive(1);
        const Unit& b = *bomber.sim.find(bomber.id);
        const Unit& k = *bomber.sim.find(tank_id);
        const double dx = static_cast<double>(b.x.raw - k.x.raw) / kTile;
        const double dy = static_cast<double>(b.y.raw - k.y.raw) / kTile;
        closest = std::min(closest, (dx * dx) + (dy * dy));
    }
    std::cout << std::format("[ G18 ] bomber over a standing tank: closest {:.3f} tiles\n",
                             std::sqrt(closest));
    EXPECT_GT(std::sqrt(closest), 0.5) << "the bomber flew through the tank";
}

} // namespace
} // namespace protekt::sim
