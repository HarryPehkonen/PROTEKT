// TURRET COMMANDS in the sim (Harri, 2026-11-01): a turret deploys where it is sent in five
// seconds, un-deploys at will in three, and `Defend` drives it to within range of an enemy, deploys
// it there and lets it shoot. A deployed turret is a NEW KIND (`DeployedTurret`, appended) -- and,
// unlike the two kinds before it, one that goes BACK, so the return path is tested here as hard as
// the way out. One test per rule, each isolating its own number, so a red names the rule that
// broke.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/sim/commander.hpp"
#include "protekt/sim/observer.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/pool.hpp"
#include "protekt/sim/replay.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace {

using protekt::sim::Fixed;
using protekt::sim::Order;
using protekt::sim::Pool;
using protekt::sim::Scenario;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::Spawn;
using protekt::sim::Stance;
using protekt::sim::Unit;
using protekt::sim::UnitId;
using protekt::sim::UnitKind;
namespace orders = protekt::sim::orders;

constexpr std::uint64_t kSeed = 20261023;

/// A tile with open ground all round it (7 x 7), scanning out from (x, y): no test depends on a
/// hand-picked coordinate happening to be passable or flat enough to see across.
struct Tile {
    int x;
    int y;
};
Tile open_tile(int x, int y) {
    for (int r = 0; r < 80; ++r) {
        for (int dy = -r; dy <= r; ++dy) {
            for (int dx = -r; dx <= r; ++dx) {
                bool open = true;
                for (int oy = -3; oy <= 3 && open; ++oy) {
                    for (int ox = -3; ox <= 3 && open; ++ox) {
                        open = protekt::sim::tile_passable(kSeed, x + dx + ox, y + dy + oy);
                    }
                }
                if (open) {
                    return Tile{x + dx, y + dy};
                }
            }
        }
    }
    return Tile{x, y};
}

void run(Sim& sim, std::uint32_t ticks) {
    for (std::uint32_t i = 0; i < ticks; ++i) {
        sim.step();
    }
}

void give(Sim& sim, std::uint32_t unit, const protekt::sim::OrderPayload& payload) {
    sim.enqueue(Order{UnitId{unit}, payload});
}

constexpr std::uint32_t kTurret = 1; ///< the player's turret, always the first spawn

/// A lone player turret on open ground, and (if asked) an enemy unit `gap` tiles east of it.
struct Field {
    Sim sim;
    Tile at;
};
Field field(UnitKind enemy = UnitKind::Factory, int gap = 0) {
    const Tile at = open_tile(60, 60);
    Scenario s;
    s.seed = kSeed;
    s.spawns = {Spawn{UnitKind::Turret, Side::Player, at.x, at.y}};
    if (gap > 0) {
        s.spawns.push_back(Spawn{enemy, Side::Enemy, at.x + gap, at.y});
    }
    return Field{Sim::create(s), at};
}

// ---- the numbers -------------------------------------------------------------------------------

TEST(TurretDeploy, ADeployTakesFiveSecondsAndAnUndeployThree) {
    EXPECT_EQ(protekt::sim::kTurretDeployTicks, 5 * protekt::sim::kTicksPerSecond);
    EXPECT_EQ(protekt::sim::kTurretUndeployTicks, 3 * protekt::sim::kTicksPerSecond);
}

TEST(TurretDeploy, TheScavengerDeploysInFiveSecondsAndTheRecyclerStaysAtTen) {
    // Harri, 2026-11-01: "takes 5 seconds (150 ticks) to deploy". The recycler keeps its own 300.
    EXPECT_EQ(protekt::sim::scavenger_deploy_ticks(Side::Player), 150);
    EXPECT_EQ(protekt::sim::scavenger_deploy_ticks(Side::Enemy), 150);
    EXPECT_EQ(protekt::sim::kDeployTicks, 300);
}

TEST(TurretDeploy, TheNewKindIsAppendedAndNoValueMoved) {
    EXPECT_EQ(static_cast<int>(UnitKind::DeployedScavenger), 15);
    EXPECT_EQ(static_cast<int>(UnitKind::DeployedTurret), 16);
    EXPECT_EQ(protekt::sim::kUnitKindCount,
              35); // the recycler list four, POWER (J3), K3 seven, K6 six alien
    EXPECT_EQ(static_cast<int>(Stance::Recycle), 9);
    EXPECT_EQ(static_cast<int>(Stance::Undeploy), 10);
    EXPECT_EQ(static_cast<int>(Stance::Defend), 11);
    EXPECT_EQ(static_cast<int>(protekt::sim::OrderKind::Build), 15);
    EXPECT_EQ(static_cast<int>(protekt::sim::OrderKind::Undeploy), 16);
    EXPECT_EQ(static_cast<int>(protekt::sim::OrderKind::Defend), 17);
}

// ---- deploy ------------------------------------------------------------------------------------

TEST(TurretDeploy, ATurretStandingOnTheSpotDeploysOnExactlyTheFiveSecondTick) {
    Field f = field();
    give(f.sim, kTurret, orders::Deploy{f.at.x, f.at.y});
    run(f.sim, 149); // literal: five seconds is 150 ticks, whatever the constant is called
    const Unit before = *f.sim.find(UnitId{kTurret});
    EXPECT_EQ(before.kind, UnitKind::Turret);
    EXPECT_EQ(before.deploy_ticks, 149);
    run(f.sim, 1);
    const Unit* after = f.sim.find(UnitId{kTurret});
    EXPECT_EQ(after->kind, UnitKind::DeployedTurret) << "on the 150th tick, not before";
    EXPECT_EQ(after->id.value, kTurret);
    EXPECT_EQ(after->hp, before.hp) << "deploying neither heals nor wounds";
    EXPECT_EQ(after->deploy_ticks, 0) << "no countdown is left behind";
    EXPECT_EQ(after->stance, Stance::Idle);
}

TEST(TurretDeploy, ATurretSentToASpotDrivesThereAndThenDeploysAndStaysPut) {
    Field f = field();
    const int goal_x = f.at.x + 6;
    give(f.sim, kTurret, orders::Deploy{goal_x, f.at.y});
    run(f.sim, 400);
    const Unit* turret = f.sim.find(UnitId{kTurret});
    ASSERT_EQ(turret->kind, UnitKind::DeployedTurret);
    EXPECT_LT(std::abs(turret->x.floor_int() - goal_x), 2) << "it deployed AT the spot";
    const Fixed x = turret->x;
    const Fixed y = turret->y;
    run(f.sim, 60);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->x, x) << "a deployed turret never moves";
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->y, y);
}

TEST(TurretDeploy, OnlyATurretDeploysOnASpotAndTheSpotMustBeGround) {
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.spawns = {Spawn{UnitKind::Turret, Side::Player, at.x, at.y},
                Spawn{UnitKind::Tank, Side::Player, at.x + 2, at.y}};
    Sim sim = Sim::create(s);
    give(sim, 2, orders::Deploy{at.x, at.y}); // a tank has nothing to deploy into
    give(sim, 1, orders::Deploy{-1, at.y});   // off the map
    run(sim, 5);
    EXPECT_EQ(sim.find(UnitId{2})->stance, Stance::Idle);
    EXPECT_EQ(sim.find(UnitId{1})->stance, Stance::Idle);
    EXPECT_TRUE(sim.command_log().empty()) << "refused at the door, so never in a replay";
}

TEST(TurretDeploy, ADeployedTurretShootsAnEnemyInRange) {
    Field f = field(UnitKind::Factory, 10);
    give(f.sim, kTurret, orders::Deploy{f.at.x, f.at.y});
    run(f.sim, protekt::sim::kTurretDeployTicks);
    ASSERT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret);
    const std::uint16_t factory_hp = f.sim.find(UnitId{2})->hp;
    run(f.sim, 120);
    EXPECT_LT(f.sim.find(UnitId{2})->hp, factory_hp) << "the gun is up: it fires without an order";
}

// ---- un-deploy: the way back -------------------------------------------------------------------

/// A deployed turret with an enemy factory in range, so firing (or not) is observable.
Field deployed(int gap = 10) {
    Field f = field(UnitKind::Factory, gap);
    give(f.sim, kTurret, orders::Deploy{f.at.x, f.at.y});
    run(f.sim, protekt::sim::kTurretDeployTicks);
    return f;
}

TEST(TurretUndeploy, ItTakesThreeSecondsAndReturnsTheSameUnitAsAPlainTurret) {
    Field f = deployed();
    ASSERT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret);
    const Unit before = *f.sim.find(UnitId{kTurret});
    give(f.sim, kTurret, orders::Undeploy{});
    run(f.sim, 89); // literal: three seconds is 90 ticks
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret)
        << "still coming off its mount on the 89th tick";
    run(f.sim, 1);
    const Unit* after = f.sim.find(UnitId{kTurret});
    EXPECT_EQ(after->kind, UnitKind::Turret) << "back to the plain kind on the 90th tick";
    EXPECT_EQ(after->id, before.id) << "the SAME unit";
    EXPECT_EQ(after->hp, before.hp);
    EXPECT_EQ(after->x, before.x);
    EXPECT_EQ(after->y, before.y);
    EXPECT_EQ(after->deploy_ticks, 0);
    EXPECT_EQ(after->stance, Stance::Idle);
    EXPECT_TRUE(after->alive);
}

TEST(TurretUndeploy, TheGunIsOffItsMountForTheWholeCountdown) {
    Field f = deployed();
    // Control: with the gun up and the factory in range, shots come.
    Field control = deployed();
    std::size_t control_shots = 0;
    for (std::uint32_t i = 0; i < protekt::sim::kTurretUndeployTicks; ++i) {
        control.sim.step();
        control_shots += control.sim.shots().size();
    }
    ASSERT_GT(control_shots, 0U);

    give(f.sim, kTurret, orders::Undeploy{});
    std::size_t shots = 0;
    for (std::uint32_t i = 0; i < protekt::sim::kTurretUndeployTicks - 1; ++i) {
        f.sim.step();
        shots += f.sim.shots().size();
        EXPECT_FALSE(protekt::sim::is_mounted(*f.sim.find(UnitId{kTurret})));
    }
    EXPECT_EQ(shots, 0U) << "an un-deploying turret does not fire";
}

TEST(TurretUndeploy, ADeployedTurretThatWasToldToUndeployFinishesEvenIfToldToStop) {
    Field f = deployed();
    give(f.sim, kTurret, orders::Undeploy{});
    run(f.sim, 10);
    give(f.sim, kTurret, orders::Stop{});
    run(f.sim, protekt::sim::kTurretUndeployTicks);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::Turret) << "once begun, it completes";
}

TEST(TurretUndeploy, OnlyADeployedTurretCanBeToldToUndeploy) {
    Field f = field();
    give(f.sim, kTurret, orders::Undeploy{}); // a plain turret has nothing to undeploy
    EXPECT_TRUE(f.sim.command_log().empty());
    run(f.sim, 5);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::Turret);
}

TEST(TurretUndeploy, AMoveOrderOnADeployedTurretUndeploysItFirstAndThenItCrawls) {
    Field f = deployed();
    const Fixed x0 = f.sim.find(UnitId{kTurret})->x;
    give(f.sim, kTurret, orders::MoveTo{Fixed::from_int(f.at.x - 12), Fixed::from_int(f.at.y)});
    run(f.sim, 89);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->x, x0) << "it does not move while it undeploys";
    run(f.sim, 1);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::Turret);
    run(f.sim, 60);
    EXPECT_LT(f.sim.find(UnitId{kTurret})->x.raw, x0.raw) << "then it crawls to the point";
}

TEST(TurretUndeploy, FollowMeOnADeployedTurretUndeploysItAndItFollows) {
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.spawns = {Spawn{UnitKind::Turret, Side::Player, at.x, at.y},
                Spawn{UnitKind::Tank, Side::Player, at.x + 14, at.y}};
    Sim sim = Sim::create(s);
    give(sim, 1, orders::Deploy{at.x, at.y});
    run(sim, protekt::sim::kTurretDeployTicks);
    ASSERT_EQ(sim.find(UnitId{1})->kind, UnitKind::DeployedTurret);
    give(sim, 1, orders::Follow{UnitId{2}});
    run(sim, protekt::sim::kTurretUndeployTicks + 120);
    EXPECT_EQ(sim.find(UnitId{1})->kind, UnitKind::Turret);
    EXPECT_GT(sim.find(UnitId{1})->x.raw, Fixed::from_int(at.x + 1).raw) << "closing on the tank";
}

TEST(TurretUndeploy, HoldKeepsADeployedTurretDeployed) {
    Field f = deployed();
    const Unit* turret = f.sim.find(UnitId{kTurret});
    give(f.sim, kTurret, orders::GuardAt{turret->x, turret->y});
    run(f.sim, 200);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret)
        << "Hold is stand and fight, not a move";
}

TEST(TurretUndeploy, ADeployUndeployDeployRoundTripEndsDeployedAgainAndIsDeterministic) {
    const auto play = [] {
        Field f = field(UnitKind::Factory, 10);
        give(f.sim, kTurret, orders::Deploy{f.at.x, f.at.y});
        run(f.sim, protekt::sim::kTurretDeployTicks);
        EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret);
        give(f.sim, kTurret, orders::Undeploy{});
        run(f.sim, protekt::sim::kTurretUndeployTicks);
        EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::Turret);
        give(f.sim, kTurret, orders::Deploy{f.at.x + 3, f.at.y});
        run(f.sim, 300); // three tiles at a crawl, then five seconds standing
        EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::DeployedTurret);
        return f.sim.state_hash();
    };
    EXPECT_EQ(play(), play()) << "the kind changing both ways is still a pure function of the log";
}

TEST(TurretUndeploy, ARedeployElsewhereUndeploysThenDrivesThenDeploys) {
    Field f = deployed();
    give(f.sim, kTurret, orders::Deploy{f.at.x - 8, f.at.y});
    run(f.sim, protekt::sim::kTurretUndeployTicks);
    EXPECT_EQ(f.sim.find(UnitId{kTurret})->kind, UnitKind::Turret);
    run(f.sim, 300);
    const Unit* turret = f.sim.find(UnitId{kTurret});
    EXPECT_EQ(turret->kind, UnitKind::DeployedTurret);
    EXPECT_LT(std::abs(turret->x.floor_int() - (f.at.x - 8)), 2);
}

TEST(TurretUndeploy, ADeployedScavengerStillTakesNoOrderAtAll) {
    // The OTHER rule, deliberately different (Harri): the scavenger's 20 of capacity is committed
    // for as long as it lives. The turret's new order must not leak onto it.
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.pools = {Pool{at.x, at.y}};
    s.spawns = {Spawn{UnitKind::Scavenger, Side::Player, at.x, at.y}};
    Sim sim = Sim::create(s);
    give(sim, 1, orders::Deploy{at.x, at.y});
    run(sim, protekt::sim::scavenger_deploy_ticks(Side::Player) + 60);
    ASSERT_EQ(sim.find(UnitId{1})->kind, UnitKind::DeployedScavenger);
    const std::size_t logged = sim.command_log().size();
    give(sim, 1, orders::Undeploy{});
    give(sim, 1, orders::Defend{UnitId{1}});
    give(sim, 1, orders::MoveTo{Fixed::from_int(at.x + 5), Fixed::from_int(at.y)});
    EXPECT_EQ(sim.command_log().size(), logged) << "refused at the door";
    run(sim, 200);
    EXPECT_EQ(sim.find(UnitId{1})->kind, UnitKind::DeployedScavenger);
}

TEST(TurretUndeploy, TheScavengersDeployIsFiveSeconds) {
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.pools = {Pool{at.x, at.y}};
    s.spawns = {Spawn{UnitKind::Scavenger, Side::Player, at.x, at.y}};
    Sim sim = Sim::create(s);
    give(sim, 1, orders::Deploy{at.x, at.y});
    run(sim, 149);
    EXPECT_EQ(sim.find(UnitId{1})->kind, UnitKind::Scavenger);
    run(sim, 1);
    EXPECT_EQ(sim.find(UnitId{1})->kind, UnitKind::DeployedScavenger) << "the 150th tick";
}

// ---- defend ------------------------------------------------------------------------------------

TEST(TurretDefend, DrivesUntilWithinWeaponRangeOfTheEnemyThenDeploysThereAndShoots) {
    Field f = field(UnitKind::Factory, 30);
    const UnitId enemy{2};
    give(f.sim, kTurret, orders::Defend{enemy});
    const std::int64_t range = protekt::sim::spec_of(UnitKind::Turret).range().raw;
    ASSERT_EQ(range, protekt::sim::kTurretRangeRaw)
        << "within range means the turret's own 16 tiles";

    std::int64_t gap2_at_flip = -1;
    for (int i = 0; i < 1200 && gap2_at_flip < 0; ++i) {
        f.sim.step();
        const Unit* turret = f.sim.find(UnitId{kTurret});
        if (turret->kind == UnitKind::DeployedTurret) {
            const Unit* target = f.sim.find(enemy);
            const std::int64_t dx = static_cast<std::int64_t>(target->x.raw) - turret->x.raw;
            const std::int64_t dy = static_cast<std::int64_t>(target->y.raw) - turret->y.raw;
            gap2_at_flip = dx * dx + dy * dy;
        }
    }
    ASSERT_GE(gap2_at_flip, 0) << "it never deployed";
    EXPECT_LT(gap2_at_flip, range * range) << "deployed inside the weapon's range of the enemy";
    EXPECT_GT(gap2_at_flip, (range - (2 << 16)) * (range - (2 << 16)))
        << "and it did not drive on past the first tile that was in range";
    const std::uint16_t hp = f.sim.find(enemy)->hp;
    run(f.sim, 120);
    EXPECT_LT(f.sim.find(enemy)->hp, hp) << "and started shooting";
}

TEST(TurretDefend, AnEnemyAlreadyInRangeIsDefendedFromWhereTheTurretStands) {
    Field f = field(UnitKind::Factory, 10);
    const Fixed x = f.sim.find(UnitId{kTurret})->x;
    give(f.sim, kTurret, orders::Defend{UnitId{2}});
    run(f.sim, protekt::sim::kTurretDeployTicks);
    const Unit* turret = f.sim.find(UnitId{kTurret});
    EXPECT_EQ(turret->kind, UnitKind::DeployedTurret) << "five seconds, exactly, with no drive";
    EXPECT_EQ(turret->x, x);
}

TEST(TurretDefend, ATargetThatDiesOnTheWayLeavesThePlainTurretIdle) {
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.spawns = {Spawn{UnitKind::Turret, Side::Player, at.x, at.y},
                Spawn{UnitKind::Tank, Side::Enemy, at.x + 30, at.y},
                Spawn{UnitKind::Tank, Side::Player, at.x + 30, at.y + 1}};
    Sim sim = Sim::create(s);
    give(sim, 1, orders::Defend{UnitId{2}});
    run(sim, 20);
    ASSERT_EQ(sim.find(UnitId{1})->stance, Stance::Defend);
    // The enemy is killed by the player's tank standing beside it.
    give(sim, 3, orders::Attack{UnitId{2}});
    for (int i = 0; i < 600 && sim.find(UnitId{2})->alive; ++i) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(UnitId{2})->alive);
    run(sim, 5);
    EXPECT_EQ(sim.find(UnitId{1})->kind, UnitKind::Turret);
    EXPECT_EQ(sim.find(UnitId{1})->stance, Stance::Idle) << "nothing left to defend against";
}

TEST(TurretDefend, OnlyATurretMayDefendAndNeverAgainstItself) {
    Scenario s;
    s.seed = kSeed;
    const Tile at = open_tile(60, 60);
    s.spawns = {Spawn{UnitKind::Turret, Side::Player, at.x, at.y},
                Spawn{UnitKind::Tank, Side::Player, at.x + 2, at.y},
                Spawn{UnitKind::Tank, Side::Enemy, at.x + 20, at.y}};
    Sim sim = Sim::create(s);
    give(sim, 2, orders::Defend{UnitId{3}}); // a tank attacks; it does not defend
    give(sim, 1, orders::Defend{UnitId{1}}); // itself
    give(sim, 1, orders::Defend{UnitId{}});  // nobody
    EXPECT_TRUE(sim.command_log().empty());
}

// ---- observation, replay -----------------------------------------------------------------------

TEST(TurretObservation, TheKindIsSeenChangingBothWaysAndMountedFollowsTheGun) {
    Field f = field();
    const auto view = [&f] {
        const protekt::sim::Observation seen = protekt::sim::observe(f.sim, Side::Player);
        return seen.own.front();
    };
    EXPECT_EQ(view().kind, UnitKind::Turret);
    give(f.sim, kTurret, orders::Deploy{f.at.x, f.at.y});
    run(f.sim, protekt::sim::kTurretDeployTicks);
    EXPECT_EQ(view().kind, UnitKind::DeployedTurret);
    EXPECT_TRUE(view().mounted);
    give(f.sim, kTurret, orders::Undeploy{});
    run(f.sim, 5);
    EXPECT_FALSE(view().mounted) << "the gun is coming off its mount";
    run(f.sim, protekt::sim::kTurretUndeployTicks);
    EXPECT_EQ(view().kind, UnitKind::Turret) << "and the kind is the plain one again";
    EXPECT_EQ(view().id.value, kTurret);
}

TEST(TurretReplay, ADeployUndeployDeployBattleReplaysToTheLiveHash) {
    // The live battle, on the standard skirmish; the player's first turret is the unit.
    const Scenario skirmish = protekt::sim::skirmish(7);
    UnitId turret{};
    const Sim opening = Sim::create(skirmish);
    for (const Unit& unit : opening.units()) {
        if (unit.kind == UnitKind::Turret && unit.side == Side::Player) {
            turret = unit.id;
            break;
        }
    }
    ASSERT_TRUE(turret.valid());
    const protekt::sim::Unit start = *opening.find(turret);

    protekt::sim::Replay replay;
    replay.seed = 7;
    const int tx = start.x.floor_int();
    const int ty = start.y.floor_int();
    replay.commands = {
        protekt::sim::Command{1, Order{turret, orders::Deploy{tx, ty}}},
        protekt::sim::Command{1 + protekt::sim::kTurretDeployTicks + 1,
                              Order{turret, orders::Undeploy{}}},
        protekt::sim::Command{1 + protekt::sim::kTurretDeployTicks + 1
                                  + protekt::sim::kTurretUndeployTicks + 1,
                              Order{turret, orders::Deploy{tx, ty}}},
    };
    const std::string text = protekt::sim::to_text(replay);
    const auto parsed = protekt::sim::parse(text);
    ASSERT_TRUE(parsed.has_value()) << text;

    Sim live = Sim::create(skirmish);
    std::size_t next = 0;
    const protekt::sim::Tick last = replay.commands.back().tick;
    for (protekt::sim::Tick tick = 0; tick <= last; ++tick) {
        while (next < replay.commands.size() && replay.commands.at(next).tick == tick) {
            live.enqueue(replay.commands.at(next).order);
            ++next;
        }
        live.step();
    }
    EXPECT_EQ(protekt::sim::run(parsed.value_or(protekt::sim::Replay{})), live.state_hash())
        << "the replay reproduces a battle in which the kind changed there and back";
}

} // namespace
