#include "protekt/app/map_file.hpp"
#include "protekt/sim/route.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/world/ground.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

/// THE MOVER STEPS ASIDE (G13). His words: "What about the tanks? I think they should move if
/// another friendly tank is in the way." And his ruling: "I think the mover would step aside."
///
/// Read as: THE UNIT THAT IS MOVING YIELDS TO THE ONE THAT IS STANDING. A friend in the way is
/// planned around (it joins terrain and buildings in the go-around planner's list) and slid round
/// at contact; while a way round exists it is never shoved, and never passed through.
///
/// HIS RULING ON THE FALLBACK (2026-11-25): "If there is no way around, pushing is completely
/// acceptable." So ROUTE FIRST, PUSH LAST: a mover whose plan round its friend finds NO way (a
/// friend in a one-tile corridor) shoves it, with the shove's own rules (two thirds of its step,
/// and through after `kShoveYieldTicks`). The route-around is the courtesy; the shove is the
/// guarantee that two friends never hold each other for good.
///
/// Every test runs on a flat, empty arena drawn here, so the ground is what the test says.
namespace protekt::sim {
namespace {

constexpr int kArena = 48;

[[nodiscard]] app::MapDoc arena() {
    app::MapDoc map = app::generate_map(1, kArena);
    for (std::size_t i = 0; i < map.terrain.size(); ++i) {
        map.terrain[i] = world::Terrain::Flat;
        map.heights[i] = 0;
        map.solid[i] = 0;
    }
    map.spawns.clear();
    map.volumes.clear();
    return map;
}

struct Placed {
    UnitKind kind{UnitKind::Tank};
    Side side{Side::Player};
    int x{};
    int y{};
};

/// The units, in order (ids 1, 2, ...), and a far-off enemy factory so the battle has two sides.
[[nodiscard]] Sim battle_of(const std::vector<Placed>& placed) {
    Scenario scenario{};
    scenario.seed = 0;
    for (const Placed& p : placed) {
        Spawn spawn{};
        spawn.side = p.side;
        spawn.kind = p.kind;
        spawn.tile_x = p.x;
        spawn.tile_y = p.y;
        scenario.spawns.push_back(spawn);
    }
    Spawn base{};
    base.side = Side::Enemy;
    base.kind = UnitKind::Factory;
    base.tile_x = 40;
    base.tile_y = 40;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

[[nodiscard]] double tiles(std::int32_t raw) { return static_cast<double>(raw) / Fixed::kOne; }

[[nodiscard]] std::int64_t gap2(const Unit& a, const Unit& b) {
    const std::int64_t dx = a.x.raw - b.x.raw;
    const std::int64_t dy = a.y.raw - b.y.raw;
    return (dx * dx) + (dy * dy);
}

constexpr std::int64_t kBody2 = static_cast<std::int64_t>(kBodyRadiusRaw) * kBodyRadiusRaw;

void go(Sim& sim, UnitId id, int x, int y) {
    sim.enqueue(Order{id, orders::MoveTo{Fixed::from_int(x), Fixed::from_int(y)}});
}

/// What a drive came to: the closest the mover came to the friend, how far the friend was moved
/// from where it stood, and the mover's path every `kPathEvery` ticks (printed).
struct Watch {
    std::int64_t closest2{INT64_MAX};
    std::int64_t closest_to_its_spot2{INT64_MAX}; ///< to where the friend stood at the start
    std::int64_t friend_moved2{};
    std::vector<std::pair<double, double>> path;
};
constexpr int kPathEvery = 10;

[[nodiscard]] Watch watch(Sim& sim, UnitId mover, UnitId standing, int ticks) {
    Watch w;
    const Unit start = *sim.find(standing);
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
        const Unit* m = sim.find(mover);
        const Unit* f = sim.find(standing);
        w.closest2 = std::min(w.closest2, gap2(*m, *f));
        w.closest_to_its_spot2 = std::min(w.closest_to_its_spot2, gap2(*m, start));
        w.friend_moved2 = std::max(w.friend_moved2, gap2(*f, start));
        if (tick % kPathEvery == 0) {
            w.path.emplace_back(tiles(m->x.raw), tiles(m->y.raw));
        }
    }
    std::cout << "  path of unit " << mover.value << " (every " << kPathEvery << " ticks):";
    for (const auto& [x, y] : w.path) {
        std::cout << " (" << x << ", " << y << ")";
    }
    std::cout << "\n  closest to the friend "
              << std::sqrt(static_cast<double>(w.closest2)) / Fixed::kOne
              << " tiles; the friend was moved "
              << std::sqrt(static_cast<double>(w.friend_moved2)) / Fixed::kOne << " tiles\n";
    return w;
}

} // namespace

// --------------------------------------------------------------------------- the open field

TEST(StepAside, AFriendIsNotPassedThroughTheMoverEndsBeyondIt) {
    // A tank at (10, 24) is sent to (20, 24); a friendly tank stands at (15, 24), dead in its line.
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 10, 24}, {UnitKind::Tank, Side::Player, 15, 24}});
    const UnitId mover{1};
    const UnitId standing{2};
    go(sim, mover, 20, 24);
    const Watch w = watch(sim, mover, standing, 600);
    EXPECT_GE(w.closest2, kBody2) << "the mover was inside its friend's body";
    EXPECT_GE(w.closest_to_its_spot2, kBody2)
        << "the mover drove through the spot its friend stood on";
    const Unit* m = sim.find(mover);
    EXPECT_GT(tiles(m->x.raw), 18.5) << "it got beyond the friend, to its goal";
    EXPECT_NEAR(tiles(m->y.raw), 24.0, 1.5) << "beyond it, not off to one side";
}

TEST(StepAside, AFriendIsNotShoved) {
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 10, 24}, {UnitKind::Tank, Side::Player, 15, 24}});
    go(sim, UnitId{1}, 20, 24);
    const Watch w = watch(sim, UnitId{1}, UnitId{2}, 600);
    EXPECT_EQ(w.friend_moved2, 0) << "the standing friend was carried by the mover";
}

TEST(StepAside, AFriendJustOffTheLineIsNotShovedEither) {
    // The same, with the friend off the line: sent from (10, 24) to (20, 25), the mover's straight
    // line passes half a tile from the friend at (15, 24) -- inside its body. It must not clip it.
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 10, 24}, {UnitKind::Tank, Side::Player, 15, 24}});
    go(sim, UnitId{1}, 20, 25);
    const Watch w = watch(sim, UnitId{1}, UnitId{2}, 600);
    EXPECT_GE(w.closest2, kBody2);
    EXPECT_EQ(w.friend_moved2, 0) << "the friend it grazed was carried";
    EXPECT_GT(tiles(sim.find(UnitId{1})->x.raw), 18.5);
}

TEST(StepAside, TwoFriendsHeadOnDoNotEndUpInsideEachOther) {
    // Two friends sent to each other's spot, dead on one line: both arrive, and never overlap.
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 8, 24}, {UnitKind::Tank, Side::Player, 24, 24}});
    go(sim, UnitId{1}, 24, 24);
    go(sim, UnitId{2}, 8, 24);
    std::int64_t closest2 = INT64_MAX;
    for (int tick = 0; tick < 900; ++tick) {
        sim.step();
        closest2 = std::min(closest2, gap2(*sim.find(UnitId{1}), *sim.find(UnitId{2})));
    }
    std::cout << "  closest " << std::sqrt(static_cast<double>(closest2)) / Fixed::kOne
              << " tiles\n";
    EXPECT_GE(closest2, kBody2) << "the two friends ended up inside each other";
    EXPECT_NEAR(tiles(sim.find(UnitId{1})->x.raw), 24.0, 1.5);
    EXPECT_NEAR(tiles(sim.find(UnitId{2})->x.raw), 8.0, 1.5);
}

TEST(StepAside, HisAcceptanceDrivenAtAFriendItIsNotPushedAndHeDoesNotPassThrough) {
    // He takes the helm and drives straight at a friendly tank: the helm's MoveTo is a point
    // ahead, re-stated every tick, as the game sends it. The friend does not move; he stops or
    // goes round.
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 10, 24}, {UnitKind::Tank, Side::Player, 15, 24}});
    const UnitId helm{1};
    sim.enqueue(Order{helm, orders::TakeControl{}});
    const Unit start = *sim.find(UnitId{2});
    std::int64_t closest2 = INT64_MAX;
    for (int tick = 0; tick < 200; ++tick) {
        const Unit* h = sim.find(helm);
        sim.enqueue(Order{helm, orders::MoveTo{Fixed::from_raw(h->x.raw + (3 << 16)), h->y}});
        sim.step();
        closest2 = std::min(closest2, gap2(*sim.find(helm), *sim.find(UnitId{2})));
    }
    EXPECT_GE(closest2, kBody2) << "he drove into his friend";
    EXPECT_EQ(sim.find(UnitId{2})->x.raw, start.x.raw) << "and pushed it";
    EXPECT_EQ(sim.find(UnitId{2})->y.raw, start.y.raw);
}

// ------------------------------------------------------------ the planner: a friend in a door

TEST(StepAside, AFriendInTheNearDoorIsPlannedAroundThroughTheFarOne) {
    // A wall across x = 20 from y 10 to 38 with two doors, at y 24 (in the mover's line) and y 30.
    // A friend stands IN the near door. Sliding cannot get past it -- the wall is either side -- so
    // only the PLANNER can: the friend joins the things a route goes round, and the mover takes the
    // far door. Before, it shoved the friend out through the near door.
    app::MapDoc map = arena();
    for (int y = 10; y <= 38; ++y) {
        if (y != 24 && y != 30) {
            map.solid[map.index(20, y)] = 1;
        }
    }
    const world::ScopedGround guard(app::map_ground(map));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 12, 24}, {UnitKind::Tank, Side::Player, 20, 24}});
    go(sim, UnitId{1}, 28, 24);
    const Watch w = watch(sim, UnitId{1}, UnitId{2}, 900);
    EXPECT_EQ(w.friend_moved2, 0) << "the friend in the door was shoved";
    EXPECT_GE(w.closest2, kBody2);
    const Unit* m = sim.find(UnitId{1});
    EXPECT_NEAR(tiles(m->x.raw), 28.0, 1.5) << "it came through the far door to its goal";
    EXPECT_NEAR(tiles(m->y.raw), 24.0, 1.5);
    const bool far_door = std::any_of(w.path.begin(), w.path.end(), [](const auto& p) {
        return p.first > 19.0 && p.first < 22.0 && p.second > 29.0 && p.second < 32.0;
    });
    EXPECT_TRUE(far_door) << "its path went through the far door at (20, 30)";
}

TEST(StepAside, ARouteIsNotLaidThroughTheGapBetweenTwoParkedFriends) {
    // Two friendly tanks parked a diagonal apart, at (20, 24) and (21, 25): the gap between them is
    // 1.41 tiles, and a body needs 1.5. A unit at (15, 25) is sent to (27, 24); the straight line
    // runs through that gap. Found in the suite (Build.NothingIsBuiltUntilItIsOrdered): a hauler
    // planned straight through such a gap, stepped aside into its mouth and was wedged there for
    // good -- its own tile shared with both bodies, so no plan made from there could get out. The
    // plan a unit drives on goes round STANDING friends from the start (`plan_drive`).
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of({{UnitKind::Tank, Side::Player, 15, 25},
                         {UnitKind::Tank, Side::Player, 20, 24},
                         {UnitKind::Tank, Side::Player, 21, 25}});
    go(sim, UnitId{1}, 27, 24);
    const Watch w = watch(sim, UnitId{1}, UnitId{2}, 600);
    EXPECT_EQ(w.friend_moved2, 0);
    EXPECT_GE(w.closest2, kBody2);
    EXPECT_EQ(sim.find(UnitId{3})->x.raw, 21 << 16) << "the other parked friend was moved";
    EXPECT_EQ(sim.find(UnitId{3})->y.raw, 25 << 16);
    const Unit* m = sim.find(UnitId{1});
    EXPECT_NEAR(tiles(m->x.raw), 27.0, 1.5) << "it got round the pair to its goal";
    EXPECT_NEAR(tiles(m->y.raw), 24.0, 1.5);
    EXPECT_EQ(m->route.failure, RouteFailure::None);
}

// --------------------------------------------------------- the hard case: no way round at all

/// A corridor one tile wide: the row y = 24 open from x 6 to x 33, walls on rows 23 and 25, closed
/// at x 34.
[[nodiscard]] app::MapDoc corridor() {
    app::MapDoc map = arena();
    for (int x = 5; x <= 35; ++x) {
        map.solid[map.index(x, 23)] = 1;
        map.solid[map.index(x, 25)] = 1;
    }
    map.solid[map.index(5, 24)] = 1;
    map.solid[map.index(34, 24)] = 1;
    return map;
}

/// Whether `a` and `b` stand INSIDE each other with NEITHER moving -- the one thing the fallback
/// must never leave him looking at.
[[nodiscard]] bool stuck_inside(const Unit& a, const Unit& b) {
    const bool still_a = a.vx.raw == 0 && a.vy.raw == 0;
    const bool still_b = b.vx.raw == 0 && b.vy.raw == 0;
    return gap2(a, b) < kBody2 && still_a && still_b;
}

/// A unit in the corridor (`corridor`), first driven to the corridor's middle line near `x` and
/// left standing there: a one-tile corridor is only a choke for a body on its middle (a unit spawns
/// on a tile's corner, against the wall, and another can pass it at the far wall).
void park_friend_mid_corridor(Sim& sim, UnitId id, int x = 22) {
    sim.enqueue(
        Order{id, orders::MoveTo{Fixed::from_int(x), Fixed::from_raw((24 << 16) + (1 << 15))}});
    for (int tick = 0; tick < 200; ++tick) {
        sim.step();
    }
    const Unit* f = sim.find(id);
    std::cout << "  the friend stands at (" << tiles(f->x.raw) << ", " << tiles(f->y.raw) << ")\n";
}

TEST(StepAside, AFriendInACorridorWithNoWayRoundIsPushedAndTheMoverGetsThrough) {
    // THE FALLBACK. The friend stands on the corridor's middle line; the mover, behind it, is sent
    // to the far end. There is no way round (the planner says so), so -- his ruling -- it PUSHES:
    // it does not wait, it does not give the order up, and it gets to its goal. Never are the two
    // left standing inside each other.
    const world::ScopedGround guard(app::map_ground(corridor()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 7, 24}, {UnitKind::Tank, Side::Player, 12, 24}});
    park_friend_mid_corridor(sim, UnitId{2});
    const Unit* f = sim.find(UnitId{2});
    ASSERT_EQ(f->vx.raw, 0) << "the friend is standing";
    ASSERT_GT(f->y.raw, (24 << 16) + (1 << 16) / 4) << "on the corridor's middle, not its wall";
    ASSERT_LT(f->y.raw, (25 << 16) - (1 << 16) / 4);
    go(sim, UnitId{1}, 32, 24);
    const Unit start = *sim.find(UnitId{2});
    Watch w;
    bool ever_stuck_inside = false;
    for (int tick = 0; tick < kStallTicks + 200; ++tick) {
        sim.step();
        const Unit* m = sim.find(UnitId{1});
        const Unit* g = sim.find(UnitId{2});
        w.friend_moved2 = std::max(w.friend_moved2, gap2(*g, start));
        ever_stuck_inside = ever_stuck_inside || stuck_inside(*m, *g);
        if (tick % kPathEvery == 0) {
            w.path.emplace_back(tiles(m->x.raw), tiles(m->y.raw));
        }
    }
    std::cout << "  path of unit 1 (every " << kPathEvery << " ticks):";
    for (const auto& [x, y] : w.path) {
        std::cout << " (" << x << ", " << y << ")";
    }
    const Unit* m = sim.find(UnitId{1});
    const Unit* g = sim.find(UnitId{2});
    std::cout << "\n  the friend was moved "
              << std::sqrt(static_cast<double>(w.friend_moved2)) / Fixed::kOne
              << " tiles; it ends at (" << tiles(g->x.raw) << ", " << tiles(g->y.raw) << ")\n";
    EXPECT_GT(w.friend_moved2, 0) << "with no way round, the friend was not pushed";
    EXPECT_EQ(m->route.failure, RouteFailure::None) << "it gave up instead of pushing";
    EXPECT_NEAR(tiles(m->x.raw), 32.0, 1.5) << "it did not get through to its goal";
    EXPECT_FALSE(ever_stuck_inside) << "the two stood inside each other, neither moving";
    EXPECT_GE(gap2(*m, *g), kBody2) << "they ended inside each other";
}

TEST(StepAside, TwoFriendsHeadOnInACorridorBothGetThrough) {
    // Two friends sent past each other down the one-tile corridor. Each steps aside for the other
    // where there is room; where there is none, the planner finds no way round and each pushes --
    // the shove's guarantee: both ARRIVE (neither gives up), they are never left standing inside
    // each other, and they end apart.
    // Both are first parked on the corridor's MIDDLE line (spawned on corners, against one wall,
    // they slide past each other at the walls and it is no choke at all).
    const world::ScopedGround guard(app::map_ground(corridor()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 7, 24}, {UnitKind::Tank, Side::Player, 30, 24}});
    park_friend_mid_corridor(sim, UnitId{1}, 12);
    park_friend_mid_corridor(sim, UnitId{2}, 22);
    go(sim, UnitId{1}, 30, 24);
    go(sim, UnitId{2}, 8, 24);
    bool ever_stuck_inside = false;
    for (int tick = 0; tick < kStallTicks + 300; ++tick) {
        sim.step();
        ever_stuck_inside
            = ever_stuck_inside || stuck_inside(*sim.find(UnitId{1}), *sim.find(UnitId{2}));
    }
    const Unit* a = sim.find(UnitId{1});
    const Unit* b = sim.find(UnitId{2});
    std::cout << "  A at (" << tiles(a->x.raw) << ", " << tiles(a->y.raw) << ") failure "
              << static_cast<int>(a->route.failure) << "; B at (" << tiles(b->x.raw) << ", "
              << tiles(b->y.raw) << ") failure " << static_cast<int>(b->route.failure) << "\n";
    EXPECT_FALSE(ever_stuck_inside) << "the two stood inside each other, neither moving";
    EXPECT_GE(gap2(*a, *b), kBody2) << "they ended inside each other";
    EXPECT_NEAR(tiles(a->x.raw), 30.0, 1.5) << "A did not get through";
    EXPECT_NEAR(tiles(b->x.raw), 8.0, 1.5) << "B did not get through";
}

TEST(StepAside, AtTheHelmAFriendWithNoWayRoundIsPushed) {
    // He drives down the corridor at the friend on its middle line. Nothing goes round it, so he
    // is not trapped behind it: he pushes it (Harri, 2026-11-03: "That way I won't get trapped").
    const world::ScopedGround guard(app::map_ground(corridor()));
    Sim sim = battle_of(
        {{UnitKind::Tank, Side::Player, 7, 24}, {UnitKind::Tank, Side::Player, 12, 24}});
    park_friend_mid_corridor(sim, UnitId{2});
    const UnitId helm{1};
    sim.enqueue(Order{helm, orders::TakeControl{}});
    const Unit start = *sim.find(UnitId{2});
    bool ever_stuck_inside = false;
    for (int tick = 0; tick < 300; ++tick) {
        const Unit* h = sim.find(helm);
        sim.enqueue(Order{helm, orders::MoveTo{Fixed::from_raw(h->x.raw + (3 << 16)),
                                               Fixed::from_raw((24 << 16) + (1 << 15))}});
        sim.step();
        ever_stuck_inside
            = ever_stuck_inside || stuck_inside(*sim.find(helm), *sim.find(UnitId{2}));
    }
    const Unit* h = sim.find(helm);
    std::cout << "  the helm ends at (" << tiles(h->x.raw) << ", " << tiles(h->y.raw)
              << "); the friend at (" << tiles(sim.find(UnitId{2})->x.raw) << ")\n";
    EXPECT_GT(gap2(*sim.find(UnitId{2}), start), 0) << "he was trapped behind his friend";
    EXPECT_GT(tiles(h->x.raw), tiles(start.x.raw)) << "he did not get past where it stood";
    EXPECT_FALSE(ever_stuck_inside);
}

// --------------------------------------------------------------- what still shoves: an enemy

TEST(StepAside, AnEnemyInTheWayIsStillShoved) {
    // The rule is about FRIENDS. A vehicle of the other side in the way is shoved as before (two
    // unarmed constructors, so nothing is shot).
    const world::ScopedGround guard(app::map_ground(arena()));
    Sim sim = battle_of({{UnitKind::Constructor, Side::Player, 10, 24},
                         {UnitKind::Constructor, Side::Enemy, 13, 24}});
    const Unit start = *sim.find(UnitId{2});
    go(sim, UnitId{1}, 25, 24);
    int shoved = 0;
    for (int tick = 0; tick < 120; ++tick) {
        sim.step();
        shoved += sim.find(UnitId{1})->shove_ticks != 0 ? 1 : 0;
    }
    EXPECT_GT(shoved, 0) << "the enemy was not shoved";
    EXPECT_GT(gap2(*sim.find(UnitId{2}), start),
              static_cast<std::int64_t>(Fixed::kOne) * Fixed::kOne);
}

} // namespace protekt::sim
