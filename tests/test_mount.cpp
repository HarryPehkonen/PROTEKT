#include "protekt/sim/commander.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// Turrets are deployable: unmounted they crawl but cannot fire; after kMountTicks of unbroken
/// stillness they are mounted and fire; any move unmounts at once. Tanks are unaffected.
namespace protekt::sim {
namespace {

struct Spot {
    Side side;
    UnitKind kind;
    int x;
    int y;
};

[[nodiscard]] Sim field(const std::vector<Spot>& spots) {
    Scenario scenario{};
    scenario.seed = 42;
    for (const Spot& s : spots) {
        Spawn spawn{};
        spawn.side = s.side;
        spawn.kind = s.kind;
        spawn.tile_x = s.x;
        spawn.tile_y = s.y;
        scenario.spawns.push_back(spawn);
    }
    return Sim::create(scenario);
}

Fixed tile(int v) { return Fixed::from_int(v); }

/// Steps one tick; reports whether `shooter` fired and whether it changed position.
struct Tick1 {
    bool fired;
    bool moved;
};
Tick1 step_watch(Sim& sim, UnitId shooter) {
    const Fixed x = sim.find(shooter)->x;
    const Fixed y = sim.find(shooter)->y;
    sim.step();
    Tick1 out{false, sim.find(shooter)->x != x || sim.find(shooter)->y != y};
    for (const Sim::Shot& shot : sim.shots()) {
        out.fired = out.fired || shot.shooter == shooter;
    }
    return out;
}

// Turret at (100,100), a harmless enemy factory 8 tiles off: in the turret's range for the whole
// of every test, and unarmed, so nothing shoots back.
const std::vector<Spot> kTurretField
    = {{Side::Player, UnitKind::Turret, 100, 100}, {Side::Enemy, UnitKind::Factory, 100, 108}};

TEST(Mount, ASpawnedTurretIsMountedAndFires) {
    Sim sim = field(kTurretField);
    const UnitId turret = sim.units()[0].id;
    EXPECT_TRUE(is_mounted(*sim.find(turret)));
    EXPECT_TRUE(observe(sim, Side::Player).own[0].mounted);
    bool fired = false;
    for (int t = 0; t < 40; ++t) {
        fired = step_watch(sim, turret).fired || fired;
    }
    EXPECT_TRUE(fired);
    EXPECT_LT(sim.units()[1].hp, kFactoryHp);
}

TEST(Mount, AMovingTurretFiresNothing) {
    Sim sim = field(kTurretField);
    const UnitId turret = sim.units()[0].id;
    sim.enqueue(Order{turret, orders::MoveTo{tile(130), tile(100)}});
    for (int t = 0; t < 120; ++t) {
        const Tick1 r = step_watch(sim, turret);
        EXPECT_TRUE(r.moved) << "tick " << t;
        EXPECT_FALSE(r.fired) << "tick " << t;
    }
    EXPECT_EQ(sim.units()[1].hp, kFactoryHp);
    EXPECT_FALSE(observe(sim, Side::Player).own[0].mounted);
}

TEST(Mount, AStoppedTurretFiresExactlyAtMountTicks) {
    Sim sim = field(kTurretField);
    const UnitId turret = sim.units()[0].id;
    sim.enqueue(Order{turret, orders::MoveTo{tile(130), tile(100)}});
    for (int t = 0; t < 20; ++t) {
        sim.step();
    }
    sim.enqueue(Order{turret, orders::Stop{}});
    int since_move = 0; // ticks since the last tick on which it moved
    int first_shot = -1;
    for (int t = 0; t < 200 && first_shot < 0; ++t) {
        const Tick1 r = step_watch(sim, turret);
        since_move = r.moved ? 0 : since_move + 1;
        if (r.fired) {
            first_shot = since_move;
        }
    }
    EXPECT_EQ(first_shot, static_cast<int>(kMountTicks));
}

TEST(Mount, MovingResetsTheClock) {
    Sim sim = field(kTurretField);
    const UnitId turret = sim.units()[0].id;
    sim.enqueue(Order{turret, orders::MoveTo{tile(130), tile(100)}});
    for (int t = 0; t < 10; ++t) {
        sim.step();
    }
    sim.enqueue(Order{turret, orders::Stop{}});
    for (int t = 0; t < 60; ++t) { // 60 still ticks: not yet mounted
        EXPECT_FALSE(step_watch(sim, turret).fired);
    }
    sim.enqueue(Order{turret, orders::MoveTo{tile(130), tile(100)}});
    for (int t = 0; t < 3; ++t) {
        sim.step();
    }
    sim.enqueue(Order{turret, orders::Stop{}});
    int since_move = 0;
    int first_shot = -1;
    for (int t = 0; t < 200 && first_shot < 0; ++t) {
        const Tick1 r = step_watch(sim, turret);
        since_move = r.moved ? 0 : since_move + 1;
        if (r.fired) {
            first_shot = since_move;
        }
    }
    EXPECT_EQ(first_shot, static_cast<int>(kMountTicks))
        << "the 60 earlier still ticks do not count";
}

TEST(Mount, TanksStillFireOnTheMove) {
    for (const UnitKind kind : {UnitKind::Tank, UnitKind::HoverTank}) {
        Sim sim
            = field({{Side::Player, kind, 100, 100}, {Side::Enemy, UnitKind::Factory, 100, 108}});
        const UnitId tank = sim.units()[0].id;
        sim.enqueue(Order{tank, orders::MoveTo{tile(130), tile(100)}});
        int shots = 0;
        for (int t = 0; t < 60; ++t) {
            const Tick1 r = step_watch(sim, tank);
            shots += r.fired && r.moved ? 1 : 0;
        }
        EXPECT_GE(shots, 2);
        EXPECT_TRUE(observe(sim, Side::Player).own[0].mounted) << "a tank is never unmounted";
    }
}

TEST(Mount, ATurretManualShotNeedsMountingToo) {
    Sim sim = field(kTurretField);
    const UnitId turret = sim.units()[0].id;
    sim.enqueue(Order{turret, orders::MoveTo{tile(130), tile(100)}});
    for (int t = 0; t < 30; ++t) {
        sim.step(); // the opening reload is long over
    }
    sim.enqueue(Order{turret, orders::FireTowards{Fixed{}, Fixed::from_int(1), UnitId{}, Fixed{}}});
    sim.step();
    EXPECT_TRUE(sim.projectiles().empty());
}

TEST(Mount, TwoSameSeedRunsAgree) {
    auto run = [] {
        Sim sim = field(kTurretField);
        const UnitId turret = sim.units()[0].id;
        sim.enqueue(Order{turret, orders::MoveTo{tile(120), tile(100)}});
        for (int t = 0; t < 400; ++t) {
            if (t == 100) {
                sim.enqueue(Order{turret, orders::Stop{}});
            }
            sim.step();
        }
        return sim.state_hash();
    };
    EXPECT_EQ(run(), run());
}

} // namespace
} // namespace protekt::sim
