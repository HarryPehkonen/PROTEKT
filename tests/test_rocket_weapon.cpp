#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>

/// THE ROCKET AS A CARRIED WEAPON (B). Harri asked for it to reach further than the gun he fires it
/// from (2026-10-27), which is only possible if the rocket has numbers of its own: `FireTowards`
/// reads the SHOOTER's kind, so before `FireRocket` existed a rocket fired from a tank was a tank
/// bolt wearing a rocket crosshair. Every number here is read from the spec or the sim under test,
/// so there is no second table to drift from.
namespace protekt::sim {
namespace {

/// A lone player tank, far from anything hostile, with its opening reload finished. Nothing hostile
/// exists, so the sim's own auto-engagement can never fire and every shot counted here is the one
/// the test asked for.
[[nodiscard]] Sim reloaded_lone_tank() {
    Scenario s;
    s.seed = 7;
    s.spawns = {{UnitKind::Tank, Side::Player, 20, 20}};
    Sim sim = Sim::create(s);
    for (int t = 0; t < profile(UnitKind::Tank).cooldown_ticks; ++t) {
        sim.step();
    }
    return sim;
}

[[nodiscard]] UnitId only_unit(const Sim& sim) { return sim.units().front().id; }

TEST(RocketWeapon, TheCarriedRocketOutreachesTheHullAndEveryGunInTheGame) {
    const UnitSpec& gun = spec_of(UnitKind::Tank);
    EXPECT_GT(kRocketSpec.range.raw, gun.range.raw)
        << "the ask: further than the gun that fires it";
    EXPECT_GT(kRocketSpec.range.raw, kTurretRangeRaw) << "the longest reach there is, past turrets";
    EXPECT_GT(kRocketSpec.max_travel.raw, gun.max_travel.raw)
        << "and the bolt actually flies there";
}

TEST(RocketWeapon, TheRocketKeepsItsOwnRoundRatherThanTheHulls) {
    // A rocket is not a gun: three times the tank's damage at a fifth of its rate, and a slower
    // bolt. If these ever equal the hull's, the rocket has quietly become a second gun.
    const UnitSpec& gun = spec_of(UnitKind::Tank);
    EXPECT_EQ(kRocketSpec.damage, gun.damage * 3);
    EXPECT_EQ(kRocketSpec.cooldown, gun.cooldown * 5);
    EXPECT_LT(kRocketSpec.bolt_speed.raw, gun.bolt_speed.raw);
    EXPECT_EQ(kRocketSpec.pellets, 0) << "one bolt, no fan";
    EXPECT_EQ(kRocketSpec.splash_radius.raw, 0) << "single target";
    EXPECT_EQ(kRocketSpec.gravity.raw, 0) << "a straight flight";
}

TEST(RocketWeapon, AFireRocketOrderFiresOneBoltWithTheRocketsNumbers) {
    Sim sim = reloaded_lone_tank();
    const UnitId mine = only_unit(sim);
    ASSERT_TRUE(sim.projectiles().empty());

    sim.enqueue(Order{mine, orders::FireRocket{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();

    ASSERT_EQ(sim.projectiles().size(), 1U) << "one bolt: the rocket does not fan";
    const Projectile& bolt = sim.projectiles().front();
    EXPECT_EQ(bolt.shooter, mine);
    EXPECT_FALSE(bolt.aimed_at.valid());
    EXPECT_EQ(bolt.damage, kRocketSpec.damage) << "the WEAPON's damage, not the hull's";
    EXPECT_EQ(bolt.max_travel.raw, kRocketSpec.max_travel.raw) << "and the weapon's reach";
    EXPECT_GT(bolt.vy.raw, 0) << "asked for due south";
    EXPECT_EQ(sim.find(mine)->stance, Stance::Idle) << "a shot is not a movement order";
}

TEST(RocketWeapon, TheRocketDeclinesASecondShotAndAnswersWhenTheReloadIsDone) {
    Sim sim = reloaded_lone_tank();
    const UnitId mine = only_unit(sim);
    sim.enqueue(Order{mine, orders::FireRocket{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();
    ASSERT_EQ(sim.shots().size(), 1U) << "the rocket went out";

    // A fifth of the gun's rate, so a second trigger ten ticks later is still declined. The count
    // is read from the sim's own SHOT record for the step, not from the bolt list: the first bolt
    // is still in the air here, and a list that fizzles bolts would make the count ambiguous.
    const std::uint16_t after_firing = sim.find(mine)->cooldown;
    EXPECT_GT(after_firing, spec_of(UnitKind::Tank).cooldown)
        << "a much longer reload than the gun";
    for (int t = 0; t < 10; ++t) {
        sim.step();
    }
    sim.enqueue(Order{mine, orders::FireRocket{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();
    EXPECT_TRUE(sim.shots().empty()) << "declined: the rocket is still reloading";

    // And it answers once the reload has run out.
    for (std::uint16_t t = 0; t < kRocketSpec.cooldown; ++t) {
        sim.step();
    }
    sim.enqueue(Order{mine, orders::FireRocket{Fixed::zero(), Fixed::from_int(1)}});
    sim.step();
    EXPECT_EQ(sim.shots().size(), 1U) << "the reload is done and the launcher answers";
}

TEST(RocketWeapon, TheRocketBoltFliesFartherThanTheHullGunBolt) {
    // Reach, measured rather than asserted from a table: two identical sims, one shot each along
    // the same ray, stepped until each bolt leaves the air. This is the number Harri will feel.
    Sim gun_sim = reloaded_lone_tank();
    Sim rocket_sim = reloaded_lone_tank();
    const UnitId gun_id = only_unit(gun_sim);
    const UnitId rocket_id = only_unit(rocket_sim);
    const Fixed start = gun_sim.find(gun_id)->y;
    gun_sim.enqueue(Order{gun_id, orders::FireTowards{Fixed::zero(), Fixed::from_int(1)}});
    rocket_sim.enqueue(Order{rocket_id, orders::FireRocket{Fixed::zero(), Fixed::from_int(1)}});
    gun_sim.step();
    rocket_sim.step();
    ASSERT_EQ(gun_sim.projectiles().size(), 1U);
    ASSERT_EQ(rocket_sim.projectiles().size(), 1U) << "the rocket's own order must fire one bolt";

    Fixed gun_end = start;
    Fixed rocket_end = start;
    for (int t = 0; t < 2000 && !gun_sim.projectiles().empty(); ++t) {
        gun_end = gun_sim.projectiles().front().y;
        gun_sim.step();
    }
    for (int t = 0; t < 2000 && !rocket_sim.projectiles().empty(); ++t) {
        rocket_end = rocket_sim.projectiles().front().y;
        rocket_sim.step();
    }
    EXPECT_GT(rocket_end.raw - start.raw, gun_end.raw - start.raw)
        << "the rocket reaches farther down the same ray than the gun";
}

} // namespace
} // namespace protekt::sim
