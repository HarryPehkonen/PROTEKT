#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cstdint>

/// Friendly fire for explicit orders only: a named target may be on your own side, but the gun
/// never picks a friendly on its own.
///
/// Targets are FACTORIES (unarmed), so the tank under test is never shot back at and every hp
/// change below is its own doing.
namespace protekt::sim {
namespace {

struct Field {
    Sim sim;
    UnitId shooter{};
    UnitId friendly{};
    UnitId hostile{};
};

/// Player tank at (100,100); a player factory three tiles east; and, when `with_hostile`, an enemy
/// factory five tiles east. All inside the tank's weapon range.
[[nodiscard]] Field field(bool with_hostile) {
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
    add(Side::Player, UnitKind::Factory, 103);
    if (with_hostile) {
        add(Side::Enemy, UnitKind::Factory, 105);
    }
    Field out{};
    out.sim = Sim::create(scenario);
    for (const Unit& unit : out.sim.units()) {
        if (unit.kind == UnitKind::Tank) {
            out.shooter = unit.id;
        } else if (unit.side == Side::Player) {
            out.friendly = unit.id;
        } else {
            out.hostile = unit.id;
        }
    }
    return out;
}

void run(Sim& sim, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
}

std::uint16_t full_hp(const Sim& sim, UnitId id) { return profile(sim.find(id)->kind).hp; }

TEST(FriendlyFire, ATriggerNamedFriendlyTargetTakesDamage) {
    Field f = field(false);
    const std::uint16_t before = f.sim.find(f.friendly)->hp;
    f.sim.enqueue(Order{f.shooter, orders::FocusFire{f.friendly}});
    run(f.sim, 90);
    EXPECT_LT(f.sim.find(f.friendly)->hp, before);
}

TEST(FriendlyFire, AnExplicitAttackOrderOnAFriendlyTakesDamage) {
    Field f = field(false);
    const std::uint16_t before = f.sim.find(f.friendly)->hp;
    f.sim.enqueue(Order{f.shooter, orders::Attack{f.friendly}});
    run(f.sim, 90);
    EXPECT_LT(f.sim.find(f.friendly)->hp, before);
}

TEST(FriendlyFire, AutoEngagementNeverTargetsAFriendly) {
    Field f = field(false);
    const std::uint16_t before = f.sim.find(f.friendly)->hp;
    run(f.sim, 600);
    EXPECT_EQ(f.sim.find(f.friendly)->hp, before);
    EXPECT_EQ(f.sim.find(f.shooter)->hp, full_hp(f.sim, f.shooter));
}

TEST(FriendlyFire, ExplicitOrdersOnAHostileStillWork) {
    Field focus = field(true);
    focus.sim.enqueue(Order{focus.shooter, orders::FocusFire{focus.hostile}});
    run(focus.sim, 90);
    EXPECT_LT(focus.sim.find(focus.hostile)->hp, full_hp(focus.sim, focus.hostile));

    Field attack = field(true);
    attack.sim.enqueue(Order{attack.shooter, orders::Attack{attack.hostile}});
    run(attack.sim, 90);
    EXPECT_LT(attack.sim.find(attack.hostile)->hp, full_hp(attack.sim, attack.hostile));
}

TEST(FriendlyFire, AFriendlyKilledByExplicitFireDies) {
    Field f = field(false);
    // One click is one shot now, so destroying a friendly takes a click per shot, as a player's
    // held button would send them.
    for (int tick = 0; tick < 3000; ++tick) {
        f.sim.enqueue(Order{f.shooter, orders::FocusFire{f.friendly}});
        f.sim.step();
    }
    const Unit* dead = f.sim.find(f.friendly);
    ASSERT_NE(dead, nullptr);
    EXPECT_FALSE(dead->alive);
    EXPECT_EQ(dead->hp, 0);
    EXPECT_EQ(dead->stance, Stance::Dead);
}

TEST(FriendlyFire, AUnitCannotTargetItself) {
    // A self-target is on the same side, so relaxing the side filter would otherwise make it a
    // legal order -- and a unit is at distance zero from itself, in range by definition. The order
    // must be REJECTED, not merely harmless: assert no target is taken, as well as no damage.
    Field focus = field(false);
    const std::uint16_t before = focus.sim.find(focus.shooter)->hp;
    focus.sim.enqueue(Order{focus.shooter, orders::FocusFire{focus.shooter}});
    run(focus.sim, 300);
    EXPECT_NE(focus.sim.find(focus.shooter)->focus_target, focus.shooter);
    EXPECT_EQ(focus.sim.find(focus.shooter)->hp, before);
    EXPECT_EQ(focus.sim.find(focus.friendly)->hp, full_hp(focus.sim, focus.friendly));

    Field attack = field(false);
    attack.sim.enqueue(Order{attack.shooter, orders::Attack{attack.shooter}});
    run(attack.sim, 300);
    EXPECT_NE(attack.sim.find(attack.shooter)->target, attack.shooter);
    EXPECT_EQ(attack.sim.find(attack.shooter)->hp, full_hp(attack.sim, attack.shooter));
}

TEST(FriendlyFire, AutoEngagementStillPrefersAHostileOverAFriendlyInRange) {
    Field f = field(true);
    const std::uint16_t friendly_before = f.sim.find(f.friendly)->hp;
    run(f.sim, 90);
    EXPECT_LT(f.sim.find(f.hostile)->hp, full_hp(f.sim, f.hostile));
    EXPECT_EQ(f.sim.find(f.friendly)->hp, friendly_before);
}

} // namespace
} // namespace protekt::sim
