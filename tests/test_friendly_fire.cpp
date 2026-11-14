#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>

/// Friendly fire for explicit orders only: a named target may be on your own side, but the gun
/// never picks a friendly on its own.
///
/// Targets are unarmed, so the tank under test is never shot back at and every hp change below is
/// its own doing. The HOSTILE is a wheeled RECYCLER (it was a factory; a factory is 8 x 8 since
/// round 19, L2, and cannot stand beside the friendly). The FRIENDLY is still a FACTORY, spawned so
/// that its centre is five tiles east (it was three): the bolts fly flat at a tank's own height
/// and only a factory's tall body is struck by one on this ground.
namespace protekt::sim {
namespace {

/// The north-west tile of a row of `length` + 1 passable tiles whose ground stays within four units
/// of the first's: a flat bolt, level with its muzzle, is then level with whatever stands on the
/// row. (The ground at (100, 100) falls away to the east, so a flat bolt flew over a unit a few
/// tiles out.)
struct RowAt {
    int x{};
    int y{};
};
[[nodiscard]] RowAt level_row(int length) {
    const auto height = [](int x, int y) {
        return static_cast<int>(ground_height(42, Fixed::from_int(x), Fixed::from_int(y)).raw
                                / Fixed::kOne);
    };
    for (int y = 40; y < kMapTiles - 40; ++y) {
        for (int x = 40; x + length < kMapTiles - 40; ++x) {
            bool level = true;
            for (int i = 0; i <= length && level; ++i) {
                level = std::abs(height(x + i, y) - height(x, y)) <= 4;
            }
            // And open ground for the 8 x 8 buildings that stand astride the row (four tiles to
            // either side, and four beyond the last tile), so none is moved off it.
            for (int ty = y - 5; ty <= y + 4 && level; ++ty) {
                for (int tx = x - 1; tx <= x + length + 5 && level; ++tx) {
                    level = tile_passable(42, tx, ty);
                }
            }
            if (level) {
                return RowAt{x, y};
            }
        }
    }
    return RowAt{-1, -1};
}

struct Field {
    Sim sim;
    UnitId shooter{};
    UnitId friendly{};
    UnitId hostile{};
};

/// Player tank at the start of a level row; a player factory (8 x 8 since round 19, L2) centred
/// five tiles east; and, when `with_hostile`, an enemy recycler four tiles due SOUTH of the tank --
/// off the line to the friendly, because a bolt hits a friendly in its way and one standing between
/// would shield the hostile (and, since round 19, L4, a building's whole FOOTPRINT shields: the
/// factory covers the rows from four north of the tank to three south of it, so the hostile stands
/// below them, at the tank's own column, west of the footprint). All inside the tank's weapon
/// range.
[[nodiscard]] Field field(bool with_hostile) {
    Scenario scenario{};
    scenario.seed = 42;
    const RowAt row = level_row(14);
    const auto add = [&](Side side, UnitKind kind, int tile_x, int tile_y) {
        Spawn spawn{};
        spawn.side = side;
        spawn.kind = kind;
        spawn.tile_x = tile_x;
        spawn.tile_y = tile_y;
        scenario.spawns.push_back(spawn);
    };
    add(Side::Player, UnitKind::Tank, row.x, row.y);
    add(Side::Player, UnitKind::Factory, row.x + 1, row.y - 4);
    if (with_hostile) {
        add(Side::Enemy, UnitKind::Recycler, row.x, row.y + 4);
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

/// The player's trigger: a bolt along the aim, with the unit under the crosshair named as intent.
/// The friendly is due east of the shooter in `field`, so east is "the crosshair on it".
[[nodiscard]] Order aimed_shot(const Field& f, UnitId aimed_at) {
    return Order{f.shooter, orders::FireTowards{Fixed::from_int(1), Fixed::zero(), aimed_at}};
}

TEST(FriendlyFire, ATriggerNamedFriendlyTargetTakesDamage) {
    Field f = field(false);
    const std::uint16_t before = f.sim.find(f.friendly)->hp;
    run(f.sim, profile(UnitKind::Tank).cooldown_ticks); // a new gun is still reloading
    f.sim.enqueue(aimed_shot(f, f.friendly));
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
    // One click is one shot, so destroying a friendly takes a click per shot, as a player's held
    // button would send them -- each one aimed along the crosshair at it, which is what names it.
    for (int tick = 0; tick < 3000; ++tick) {
        f.sim.enqueue(aimed_shot(f, f.friendly));
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
