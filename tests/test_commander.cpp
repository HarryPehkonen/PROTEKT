// The commander interface is the seam the whole game hangs on: the human's input layer, the
// scripted opponent and (later) the AI all arrive here, so it must be narrow and honest about
// what a commander can see.
#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <variant>
#include <vector>

#include "protekt/ai/scripted.hpp"
#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

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
    ASSERT_EQ(orders.size(), 6U)
        << "3 tanks and the hover tank ordered to attack, 2 turrets ordered to guard";

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
    EXPECT_EQ(attacks, 4);
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

} // namespace
