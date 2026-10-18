#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// Fire control: choosing WHAT to shoot, as distinct from where to drive.
///
/// The gun already fires on its own at the nearest hostile in range, so the only thing a trigger
/// can add is a CHOICE. That choice arrives as an order like every other decision in this
/// simulation, and it deliberately sets nothing but a target: a unit the player is driving is
/// steered by the player's keys every tick, so an order that also moved the unit would fight the
/// keyboard.
///
/// The enemy units here are FACTORIES on purpose -- they are unarmed, so the tank under test is
/// never shot at, and every damage figure below is the tank's own doing rather than a firefight.
namespace protekt::sim {
namespace {

inline constexpr std::uint16_t kFactoryHp = 400;

struct Field {
    Sim sim;
    UnitId mine{};
    UnitId near_enemy{};
    UnitId far_enemy{};
};

/// One player tank at (100,100) and two enemy factories at three and ten tiles east. Both are
/// inside the tank's twelve-tile weapon range, so which one gets shot is a real choice and not a
/// geometry test.
[[nodiscard]] Field field() {
    Scenario scenario{};
    scenario.seed = 42;
    const auto add = [&](Side side, UnitKind kind, int tile_x) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = tile_x;
        spawn.tile_y = 100;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Player, UnitKind::Tank, 100);
    add(Side::Enemy, UnitKind::Factory, 103);
    add(Side::Enemy, UnitKind::Factory, 110);

    Field out{};
    out.sim = Sim::create(scenario);
    for (const Unit& unit : out.sim.units()) {
        if (unit.side == Side::Player) {
            out.mine = unit.id;
        } else if (unit.x.floor_int() == 103) {
            out.near_enemy = unit.id;
        } else {
            out.far_enemy = unit.id;
        }
    }
    return out;
}

void focus_fire(Sim& sim, UnitId mine, UnitId target) {
    sim.enqueue(Order{mine, orders::FocusFire{target}});
    sim.step();
}

void run(Sim& sim, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
}

TEST(FocusFire, TheNamedTargetIsShotAndTheNearerOneIsNot) {
    Field f = field();
    focus_fire(f.sim, f.mine, f.far_enemy);
    run(f.sim, 90);

    EXPECT_LT(f.sim.find(f.far_enemy)->hp, kFactoryHp) << "the named target was not shot";
    EXPECT_EQ(f.sim.find(f.near_enemy)->hp, kFactoryHp)
        << "the nearer hostile was shot instead, so the order did nothing";
}

TEST(FocusFire, ItChangesNeitherStanceNorMovement) {
    // The property that makes this usable as a trigger: this is a unit the player is driving.
    Field f = field();
    const Order move{f.mine,
                     orders::MoveTo{Fixed::from_raw(static_cast<std::int32_t>(
                                        static_cast<std::uint32_t>(120) << Fixed::kShift)),
                                    Fixed::from_raw(static_cast<std::int32_t>(
                                        static_cast<std::uint32_t>(100) << Fixed::kShift))}};
    f.sim.enqueue(move);
    f.sim.step();

    const Fixed before_x = f.sim.find(f.mine)->x;
    focus_fire(f.sim, f.mine, f.far_enemy);

    EXPECT_EQ(f.sim.find(f.mine)->stance, Stance::MoveTo)
        << "the order changed what the unit is doing";
    run(f.sim, 90);
    EXPECT_GT(f.sim.find(f.mine)->x.raw, before_x.raw) << "and it stopped driving";
}

TEST(FocusFire, AnIdleUnitStillShootsWhatItWasTold) {
    // The player's case: no stance, no orders to move, driving on keys -- and a trigger.
    Field f = field();
    EXPECT_EQ(f.sim.find(f.mine)->stance, Stance::Idle);
    focus_fire(f.sim, f.mine, f.far_enemy);
    run(f.sim, 90);

    EXPECT_LT(f.sim.find(f.far_enemy)->hp, kFactoryHp);
    EXPECT_EQ(f.sim.find(f.near_enemy)->hp, kFactoryHp);
}

TEST(FocusFire, StopClearsItAndTheGunGoesBackToTheNearest) {
    Field f = field();
    focus_fire(f.sim, f.mine, f.far_enemy);
    run(f.sim, 90);
    EXPECT_LT(f.sim.find(f.far_enemy)->hp, kFactoryHp);
    const std::uint16_t far_reached = f.sim.find(f.far_enemy)->hp;

    f.sim.enqueue(Order{f.mine, orders::Stop{}});
    f.sim.step();
    run(f.sim, 150);

    EXPECT_EQ(f.sim.find(f.far_enemy)->hp, far_reached)
        << "the gun kept firing at the unit the trigger named, after Stop";
    EXPECT_LT(f.sim.find(f.near_enemy)->hp, kFactoryHp)
        << "with the focus cleared it must go back to engaging the nearest hostile in range";
}

TEST(FocusFire, WhenTheNamedTargetDiesTheGunFallsBackToTheNearest) {
    Field f = field();
    focus_fire(f.sim, f.mine, f.far_enemy);
    run(f.sim, 1200);

    EXPECT_FALSE(f.sim.find(f.far_enemy)->alive) << "the named target should have been destroyed";
    EXPECT_LT(f.sim.find(f.near_enemy)->hp, kFactoryHp)
        << "with the named target gone the gun must go back to engaging what is in range";
}

} // namespace
} // namespace protekt::sim
