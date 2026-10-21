#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

/// The spec table is the one place a kind's numbers live. Every stat of every kind is pinned here
/// to the constant the game has always used, so a wrong entry names itself.
namespace protekt::sim {
namespace {

TEST(Spec, TableCoversEveryKind) {
    EXPECT_EQ(kUnitSpecs.size(), static_cast<std::size_t>(kUnitKindCount));
}

TEST(Spec, EveryKindHasAName) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitSpec& spec = spec_of(static_cast<UnitKind>(i));
        ASSERT_NE(spec.name, nullptr) << "kind " << i;
        EXPECT_GT(std::strlen(spec.name), 0U) << "kind " << i;
    }
}

TEST(Spec, TankStats) {
    const UnitSpec& s = spec_of(UnitKind::Tank);
    EXPECT_STREQ(s.name, "tank");
    EXPECT_EQ(s.hp, kTankHp);
    EXPECT_EQ(s.max_speed.raw, kTankSpeedRaw);
    EXPECT_EQ(s.range.raw, kTankRangeRaw);
    EXPECT_EQ(s.damage, kTankDamage);
    EXPECT_EQ(s.cooldown, kTankCooldownTicks);
}

TEST(Spec, HoverTankHasTheTanksNumbers) {
    const UnitSpec& s = spec_of(UnitKind::HoverTank);
    EXPECT_STREQ(s.name, "hovertank");
    EXPECT_EQ(s.hp, kTankHp);
    EXPECT_EQ(s.max_speed.raw, kTankSpeedRaw);
    EXPECT_EQ(s.range.raw, kTankRangeRaw);
    EXPECT_EQ(s.damage, kTankDamage);
    EXPECT_EQ(s.cooldown, kTankCooldownTicks);
}

TEST(Spec, TurretStats) {
    const UnitSpec& s = spec_of(UnitKind::Turret);
    EXPECT_STREQ(s.name, "turret");
    EXPECT_EQ(s.hp, kTurretHp);
    EXPECT_EQ(s.max_speed.raw, kTurretSpeedRaw);
    EXPECT_EQ(s.range.raw, kTurretRangeRaw);
    EXPECT_EQ(s.damage, kTurretDamage);
    EXPECT_EQ(s.cooldown, kTurretCooldownTicks);
}

TEST(Spec, FactoryStats) {
    const UnitSpec& s = spec_of(UnitKind::Factory);
    EXPECT_STREQ(s.name, "factory");
    EXPECT_EQ(s.hp, kFactoryHp);
    EXPECT_EQ(s.max_speed.raw, 0);
    EXPECT_EQ(s.range.raw, 0);
    EXPECT_EQ(s.damage, 0);
    EXPECT_EQ(s.cooldown, 0);
}

TEST(Spec, IndexedByTheUnchangedEnumValues) {
    EXPECT_EQ(static_cast<int>(UnitKind::Tank), 0);
    EXPECT_EQ(static_cast<int>(UnitKind::Turret), 1);
    EXPECT_EQ(static_cast<int>(UnitKind::Factory), 2);
    EXPECT_EQ(static_cast<int>(UnitKind::HoverTank), 3);
    EXPECT_STREQ(kUnitSpecs.at(1).name, "turret");
}

TEST(Spec, ProfileIsTheTableRead) {
    for (int i = 0; i < kUnitKindCount; ++i) {
        const UnitKind kind = static_cast<UnitKind>(i);
        const UnitSpec& s = spec_of(kind);
        const UnitProfile p = profile(kind);
        EXPECT_EQ(p.hp, s.hp) << s.name;
        EXPECT_EQ(p.max_speed.raw, s.max_speed.raw) << s.name;
        EXPECT_EQ(p.weapon_range.raw, s.range.raw) << s.name;
        EXPECT_EQ(p.damage, s.damage) << s.name;
        EXPECT_EQ(p.cooldown_ticks, s.cooldown) << s.name;
        EXPECT_EQ(p.mobile, s.max_speed.raw > 0) << s.name;
    }
}

} // namespace
} // namespace protekt::sim
