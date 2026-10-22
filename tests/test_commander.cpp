// The commander interface is the seam the whole game hangs on: the human's input layer, the
// scripted opponent and (later) the AI all arrive here, so it must be narrow and honest about
// what a commander can see.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <set>
#include <variant>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace {

using namespace protekt::ai;
using namespace protekt::sim;

// Updated (R3b2): nine a side.
TEST(Commander, ObserveSplitsOwnFromEnemyAndOnlyLivingUnits) {
    Sim sim = Sim::create(skirmish(42));
    const Observation obs = observe(sim, Side::Player);
    EXPECT_EQ(obs.tick, 0U);
    EXPECT_EQ(obs.own.size(), 9U);
    EXPECT_EQ(obs.enemy.size(), 9U);
    for (const UnitView& v : obs.own) {
        EXPECT_EQ(v.side, Side::Player);
    }
    for (const UnitView& v : obs.enemy) {
        EXPECT_EQ(v.side, Side::Enemy);
        EXPECT_GT(v.hp, 0);
    }
    for (std::size_t i = 1; i < obs.own.size(); ++i) {
        EXPECT_LT(obs.own[i - 1].id.value, obs.own[i].id.value) << "ascending id order";
    }
    // Positions arrive as whole tiles, not fixed-point raws: a commander that had to know about
    // Q16.16 would be a commander coupled to the engine's internals.
    for (const UnitView& v : obs.own) {
        EXPECT_GE(v.x, 0);
        EXPECT_LT(v.x, kMapTiles);
        EXPECT_GE(v.y, 0);
        EXPECT_LT(v.y, kMapTiles);
    }
}

TEST(Commander, ObserveDropsTheDead) {
    Scenario s;
    s.seed = 5;
    s.spawns
        = {{UnitKind::Tank, Side::Player, 100, 100}, {UnitKind::Factory, Side::Enemy, 104, 100}};
    Sim sim = Sim::create(s);
    const int shots = kFactoryHp / kTankDamage;
    for (int t = 0; t < shots * kTankCooldownTicks + 60; ++t) {
        sim.step();
    }
    const Observation obs = observe(sim, Side::Player);
    EXPECT_TRUE(obs.enemy.empty()) << "a destroyed factory is not an enemy any more";
    EXPECT_EQ(obs.own.size(), 1U);
}

TEST(Commander, TheScriptedCommanderIsSilentExceptOnItsPeriod) {
    Sim sim = Sim::create(skirmish(42));
    ScriptedCommander commander(30);
    std::vector<Order> orders;
    for (Tick t = 0; t < 90; ++t) {
        orders.clear();
        commander.think(observe(sim, Side::Player), orders);
        if (t % 30 == 0) {
            EXPECT_FALSE(orders.empty()) << "it must have something to say on tick " << t;
        } else {
            EXPECT_TRUE(orders.empty()) << "it must be silent on tick " << t;
        }
        sim.step();
    }
}

TEST(Commander, IssuesOneOrderPerOwnCombatUnitAndNeverForAFactory) {
    Sim sim = Sim::create(skirmish(42));
    ScriptedCommander commander(30);
    std::vector<Order> orders;
    commander.think(observe(sim, Side::Player), orders);
    ASSERT_EQ(orders.size(), 8U) << "3 tanks, the hover tank, the mortar and the rocket ordered to "
                                    "attack, 2 turrets ordered to guard";

    const Observation obs = observe(sim, Side::Player);
    std::set<std::uint32_t> own_ids;
    for (const UnitView& v : obs.own) {
        own_ids.insert(v.id.value);
    }
    std::set<std::uint32_t> enemy_ids;
    for (const UnitView& v : obs.enemy) {
        enemy_ids.insert(v.id.value);
    }

    int attacks = 0;
    int guards = 0;
    for (const Order& o : orders) {
        EXPECT_EQ(own_ids.count(o.unit.value), 1U) << "only its own units may be ordered";
        if (o.kind() == OrderKind::Attack) {
            ++attacks;
            EXPECT_EQ(enemy_ids.count(std::get<orders::Attack>(o.payload).target.value), 1U)
                << "only a living enemy may be targeted";
        } else if (o.kind() == OrderKind::GuardAt) {
            ++guards;
        } else {
            FAIL() << "unexpected order kind " << static_cast<int>(o.kind());
        }
    }
    EXPECT_EQ(attacks, 6);
    EXPECT_EQ(guards, 2);
}

TEST(Commander, AttacksTheNearestEnemyAndBreaksTiesByLowestId) {
    Scenario s;
    s.seed = 5;
    s.spawns = {{UnitKind::Tank, Side::Player, 100, 100},
                {UnitKind::Tank, Side::Enemy, 110, 100},
                {UnitKind::Tank, Side::Enemy, 90, 100},
                {UnitKind::Tank, Side::Enemy, 130, 100}};
    Sim sim = Sim::create(s);
    const Observation obs = observe(sim, Side::Player);
    ASSERT_EQ(obs.enemy.size(), 3U);
    // Two enemies sit equidistant from the tank (10 tiles each way); the lowest id must win.
    const std::uint32_t expected = std::min(obs.enemy[0].id.value, obs.enemy[1].id.value);
    const std::uint32_t farther = obs.enemy[2].id.value;
    EXPECT_LT(expected, farther) << "the third enemy is 30 tiles away and must not be chosen";

    ScriptedCommander commander(30);
    std::vector<Order> orders;
    commander.think(obs, orders);
    ASSERT_EQ(orders.size(), 1U);
    EXPECT_EQ(orders[0].kind(), OrderKind::Attack);
    EXPECT_EQ(std::get<orders::Attack>(orders[0].payload).target.value, expected)
        << "the nearest (by tie-break, lowest id) loses";
}

TEST(Commander, ACommandedBattleIsReproducibleAndDifferentFromAnUncommandedOne) {
    const Scenario scenario = skirmish(42);
    ScriptedCommander a(30);
    ScriptedCommander b(30);
    const std::uint64_t commanded = run_battle(scenario, {}, 900, &a, &b).final_hash;
    EXPECT_EQ(commanded, run_battle(scenario, {}, 900, &a, &b).final_hash)
        << "a commanded battle must be reproducible";
    EXPECT_NE(commanded, run_battle(scenario, {}, 900, nullptr, nullptr).final_hash)
        << "orders must change the battle, or the commander is not connected to anything";

    // A commander that never speaks must be indistinguishable from no commander at all. The earlier
    // version of this test used two ScriptedCommander(30) objects as its "uncommanded" control,
    // which is not a control at all -- it was comparing a run with itself.
    ScriptedCommander silent(0);
    ScriptedCommander silent_too(0);
    EXPECT_EQ(run_battle(scenario, {}, 900, &silent, &silent_too).final_hash,
              run_battle(scenario, {}, 900, nullptr, nullptr).final_hash);
}

TEST(Commander, BothSidesActuallyReceiveOrdersInABattle) {
    // The gap this closes: every earlier test was satisfied by ONE side being commanded. The enemy
    // slot could be wired to nothing while the player fought both armies, and the suite stayed
    // green -- which is exactly what happened.
    const Scenario scenario = skirmish(42);
    Sim probe = Sim::create(scenario);
    std::set<std::uint32_t> player_ids;
    std::set<std::uint32_t> enemy_ids;
    for (const Unit& u : probe.units()) {
        (u.side == Side::Player ? player_ids : enemy_ids).insert(u.id.value);
    }
    ASSERT_FALSE(player_ids.empty());
    ASSERT_FALSE(enemy_ids.empty());

    ScriptedCommander player(30);
    ScriptedCommander enemy(30);
    const BattleLog log = run_battle(scenario, {}, 200, &player, &enemy);
    std::size_t ordered_player = 0;
    std::size_t ordered_enemy = 0;
    for (const BattleEvent& e : log.events) {
        if (e.kind != EventKind::Ordered) {
            continue;
        }
        ordered_player += player_ids.count(e.actor.value);
        ordered_enemy += enemy_ids.count(e.actor.value);
    }
    EXPECT_GT(ordered_player, 0U) << "the player's commander must be connected";
    EXPECT_GT(ordered_enemy, 0U)
        << "so must the enemy's: a side that is never ordered never fights";
}

// ---- F1: the scripted commander orders its mortar and its rocket
// ----------------------------------

[[nodiscard]] const Order* order_for_unit(const std::vector<Order>& orders, UnitId id) {
    for (const Order& o : orders) {
        if (o.unit.value == id.value) {
            return &o;
        }
    }
    return nullptr;
}

[[nodiscard]] UnitId first_of(const Sim& sim, UnitKind kind, Side side) {
    for (const Unit& u : sim.units()) {
        if (u.kind == kind && u.side == side) {
            return u.id;
        }
    }
    return UnitId{};
}

/// A seed and corner where a 34 x 6 block of tiles is all passable, so a unit spawned there is not
/// nudged onto other ground and nothing in a straight row between two of them is in the way.
struct Field {
    std::uint64_t seed{};
    int x{};
    int y{};
};

[[nodiscard]] Field open_field() {
    for (std::uint64_t seed = 1; seed < 400; ++seed) {
        for (int y = 20; y < 200; y += 5) {
            for (int x = 20; x < 200; x += 5) {
                bool clear = true;
                for (int dy = 0; dy < 6 && clear; ++dy) {
                    for (int dx = 0; dx < 34 && clear; ++dx) {
                        clear = tile_passable(seed, x + dx, y + dy);
                    }
                }
                if (clear) {
                    return Field{seed, x, y};
                }
            }
        }
    }
    return Field{};
}

[[nodiscard]] double tile_x(const Sim& sim, UnitId id) {
    return static_cast<double>(sim.find(id)->x.raw) / Fixed::kOne;
}

[[nodiscard]] double tile_y(const Sim& sim, UnitId id) {
    return static_cast<double>(sim.find(id)->y.raw) / Fixed::kOne;
}

[[nodiscard]] double tiles_between(const Sim& sim, UnitId a, UnitId b) {
    const double dx = tile_x(sim, a) - tile_x(sim, b);
    const double dy = tile_y(sim, a) - tile_y(sim, b);
    return std::sqrt(dx * dx + dy * dy);
}

TEST(ScriptedRoles, EveryCombatKindOfTheRosterReceivesAnOrderAndTheMortarAndRocketAttack) {
    Sim sim = Sim::create(skirmish(42));
    ScriptedCommander commander(30);
    std::vector<Order> orders;
    commander.think(observe(sim, Side::Player), orders);
    for (const UnitKind kind : {UnitKind::Mortar, UnitKind::Rocket}) {
        const UnitId id = first_of(sim, kind, Side::Player);
        ASSERT_TRUE(id.valid());
        const Order* order = order_for_unit(orders, id);
        ASSERT_NE(order, nullptr) << "no order for the " << static_cast<int>(kind);
        EXPECT_EQ(order->kind(), OrderKind::Attack);
    }
}

TEST(ScriptedRoles, TheMortarHoldsInsideItsRangeWhileATankInTheSameSpotAdvances) {
    // One target 13 tiles from both: inside the mortar's 14, outside the tank's 12.
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::Tank, Side::Player, f.x, f.y},
                {UnitKind::Mortar, Side::Player, f.x, f.y + 2},
                {UnitKind::Factory, Side::Enemy, f.x + 13, f.y + 1}};
    Sim sim = Sim::create(s);
    const UnitId tank = first_of(sim, UnitKind::Tank, Side::Player);
    const UnitId mortar = first_of(sim, UnitKind::Mortar, Side::Player);
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    ASSERT_GT(tiles_between(sim, tank, target), 12.0);
    ASSERT_LT(tiles_between(sim, mortar, target), 14.0);
    const double tank_before = tiles_between(sim, tank, target);
    const double mortar_before = tiles_between(sim, mortar, target);

    ScriptedCommander commander(30);
    std::vector<Order> orders;
    commander.think(observe(sim, Side::Player), orders);
    const Order* mortar_order = order_for_unit(orders, mortar);
    ASSERT_NE(mortar_order, nullptr) << "holding is a decision made with an order, not silence";
    EXPECT_EQ(mortar_order->kind(), OrderKind::Attack);
    for (const Order& o : orders) {
        sim.enqueue(o);
    }
    for (int t = 0; t < 120; ++t) {
        sim.step();
    }
    EXPECT_LT(tiles_between(sim, tank, target), tank_before - 0.5)
        << "the tank closes to its range";
    EXPECT_NEAR(tiles_between(sim, mortar, target), mortar_before, 0.01)
        << "the mortar already has the target in range and does not move";
}

TEST(ScriptedRoles, AMortarOutOfRangeAdvancesUntilTheTargetIsInRangeThenStops) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::Mortar, Side::Player, f.x, f.y},
                {UnitKind::Factory, Side::Enemy, f.x + 30, f.y}};
    Sim sim = Sim::create(s);
    const UnitId mortar = first_of(sim, UnitKind::Mortar, Side::Player);
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    const double range = static_cast<double>(spec_of(UnitKind::Mortar).range.raw) / Fixed::kOne;
    ASSERT_GT(tiles_between(sim, mortar, target), range);

    ScriptedCommander commander(30);
    std::vector<Order> orders;
    for (int t = 0; t < 900; ++t) {
        if (t % 30 == 0) {
            orders.clear();
            commander.think(observe(sim, Side::Player), orders);
            for (const Order& o : orders) {
                sim.enqueue(o);
            }
        }
        sim.step();
    }
    const double settled = tiles_between(sim, mortar, target);
    EXPECT_LE(settled, range) << "it closed until the target was in range";
    EXPECT_GT(settled, range - 2.0) << "and stopped at the edge of its range, it did not charge";
    for (int t = 0; t < 60; ++t) {
        sim.step();
    }
    EXPECT_NEAR(tiles_between(sim, mortar, target), settled, 0.01) << "and stays put";
}

TEST(ScriptedRoles, ARocketClosesLikeATankAndFiresWhenInRange) {
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::Rocket, Side::Player, f.x, f.y},
                {UnitKind::Factory, Side::Enemy, f.x + 30, f.y}};
    Sim sim = Sim::create(s);
    const UnitId rocket = first_of(sim, UnitKind::Rocket, Side::Player);
    const UnitId target = first_of(sim, UnitKind::Factory, Side::Enemy);
    const double range = static_cast<double>(spec_of(UnitKind::Rocket).range.raw) / Fixed::kOne;
    const double before = tiles_between(sim, rocket, target);
    ASSERT_GT(before, range);
    const std::uint16_t hp_before = sim.find(target)->hp;

    ScriptedCommander commander(30);
    std::vector<Order> orders;
    for (int t = 0; t < 1500; ++t) {
        if (t % 30 == 0) {
            orders.clear();
            commander.think(observe(sim, Side::Player), orders);
            for (const Order& o : orders) {
                sim.enqueue(o);
            }
        }
        sim.step();
    }
    EXPECT_LE(tiles_between(sim, rocket, target), range) << "it closed into its range";
    EXPECT_LT(sim.find(target)->hp, hp_before) << "and fired";
}

TEST(ScriptedRoles, AHurtMortarAndRocketKeepFightingBecauseThePlanHasNoRetreat) {
    // The brief called a home-when-hurt rule "existing" in the scripted plan; it is the utility
    // commander's (ai::decide). The scripted plan is a frozen fixed plan with no retreat for ANY
    // kind, so a wounded mortar or rocket is ordered exactly like a healthy one -- pinned here so
    // a retreat added later is a decision, not an accident.
    const Field f = open_field();
    ASSERT_NE(f.seed, 0U);
    Scenario s;
    s.seed = f.seed;
    s.spawns = {{UnitKind::Mortar, Side::Player, f.x + 10, f.y},
                {UnitKind::Rocket, Side::Player, f.x + 10, f.y + 2},
                {UnitKind::Factory, Side::Player, f.x, f.y},
                {UnitKind::Tank, Side::Enemy, f.x + 30, f.y}};
    Sim sim = Sim::create(s);
    Observation obs = observe(sim, Side::Player);
    for (UnitView& v : obs.own) {
        if (v.kind != UnitKind::Factory) {
            v.hp = 5;
        }
    }
    ScriptedCommander commander(30);
    std::vector<Order> orders;
    commander.think(obs, orders);
    for (const UnitKind kind : {UnitKind::Mortar, UnitKind::Rocket}) {
        const Order* order = order_for_unit(orders, first_of(sim, kind, Side::Player));
        ASSERT_NE(order, nullptr);
        EXPECT_EQ(order->kind(), OrderKind::Attack);
    }
}

TEST(ScriptedRoles, TheLiveEnemyPathOrdersItsMortarAndRocketOverRealTicks) {
    // The seam Game drives (src/app/game.cpp: enemy_.think(observe(sim, Enemy), orders) each frame,
    // then sim.enqueue) rebuilt over real ticks: the shipped bug was a unit parked at spawn.
    Sim sim = Sim::create(skirmish(42));
    const UnitId mortar = first_of(sim, UnitKind::Mortar, Side::Enemy);
    const UnitId rocket = first_of(sim, UnitKind::Rocket, Side::Enemy);
    ASSERT_TRUE(mortar.valid());
    ASSERT_TRUE(rocket.valid());
    ScriptedCommander enemy(30);
    bool mortar_ordered = false;
    bool rocket_ordered = false;
    const double rocket_start = tile_x(sim, rocket);
    const double rocket_start_y = tile_y(sim, rocket);
    for (int t = 0; t < 600; ++t) {
        std::vector<Order> orders;
        enemy.think(observe(sim, Side::Enemy), orders);
        mortar_ordered = mortar_ordered || order_for_unit(orders, mortar) != nullptr;
        rocket_ordered = rocket_ordered || order_for_unit(orders, rocket) != nullptr;
        for (const Order& o : orders) {
            sim.enqueue(o);
        }
        sim.step();
    }
    EXPECT_TRUE(mortar_ordered);
    EXPECT_TRUE(rocket_ordered);
    EXPECT_TRUE(tile_x(sim, rocket) != rocket_start || tile_y(sim, rocket) != rocket_start_y)
        << "the enemy rocket leaves its spawn";
}

TEST(ScriptedRoles, TwoSameSeedRunsAreByteIdentical) {
    const Scenario scenario = skirmish(42);
    ScriptedCommander a(30);
    ScriptedCommander b(30);
    ScriptedCommander c(30);
    ScriptedCommander d(30);
    EXPECT_EQ(run_battle(scenario, {}, 900, &a, &b).final_hash,
              run_battle(scenario, {}, 900, &c, &d).final_hash);
}

} // namespace
