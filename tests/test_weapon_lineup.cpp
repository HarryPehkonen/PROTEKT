#include "protekt/game/weapon.hpp"

#include <gtest/gtest.h>

#include <cstddef>

/// B: a vehicle's weapon LINEUP. Harri's rule is that every weapon is available to every vehicle,
/// except that some hulls will eventually be blocked from some weapons. The default is therefore
/// the whole carried set, and the block table is the seam that takes a weapon away from one kind.
/// Nothing is blocked today, so these tests pin the default and the walk that skips nothing.
namespace protekt::game {
namespace {

TEST(WeaponLineup, EveryKindCarriesEveryWeaponInClickOrder) {
    for (int raw = 0; raw < sim::kUnitKindCount; ++raw) {
        const sim::UnitKind kind = static_cast<sim::UnitKind>(raw);
        const WeaponLineup lineup = weapon_lineup(kind);
        ASSERT_EQ(lineup.count, kDrivenWeapons.size()) << "kind " << raw;
        for (std::size_t row = 0; row < lineup.count; ++row) {
            EXPECT_EQ(lineup.weapons.at(row), kDrivenWeapons.at(row))
                << "kind " << raw << " row " << row;
            EXPECT_TRUE(weapon_allowed(kind, kDrivenWeapons.at(row)))
                << "kind " << raw << " row " << row;
        }
    }
}

TEST(WeaponLineup, TheKindAwareClickWalksTheWholeSetWhileNothingIsBlocked) {
    const sim::UnitKind kind = sim::UnitKind::Tank;
    Weapon weapon = Weapon::Gun;
    for (std::size_t step = 0; step < kDrivenWeapons.size(); ++step) {
        weapon = next_weapon(kind, weapon);
        EXPECT_EQ(weapon, next_weapon(kDrivenWeapons.at(step))) << "step " << step;
    }
    EXPECT_EQ(weapon, Weapon::Gun) << "a full walk returns to the gun";
}

TEST(WeaponLineup, TheLastCarriedWeaponWrapsToTheFirst) {
    // The click's wrap, through the kind-aware path: the shotgun is last today, and the next step
    // is the gun. The same property the kind-free click has, asked of a hull.
    const WeaponLineup lineup = weapon_lineup(sim::UnitKind::Scout);
    ASSERT_GT(lineup.count, 0U);
    EXPECT_EQ(next_weapon(sim::UnitKind::Scout, lineup.weapons.at(lineup.count - 1)),
              lineup.weapons.front());
}

} // namespace
} // namespace protekt::game
