// ROUND 18, K5: THE GUN TOWER SHOOTS. The owner: "Gun Towers are like turrets, except they can't be
// moved. They are static buildings that fire very powerful weapons at the enemy."
//
// The tower rides the SAME fire step as every other gun (acquire the nearest enemy in range with a
// line, hold fire when a friend is in the lane, never an own-side target); what these tests pin is
// that it has a gun, that the gun obeys those rules, and that it is still a building: it stands
// still, it dies, a dead one is silent, and it is raised by a Constructor's job, not a queue.
//
// The numbers below are LITERALS, not read back from the table under test. The damage, the range
// and the reload are PROVISIONAL (the owner gave "very powerful" and no figures).
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

namespace protekt {
namespace {

using sim::Side;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kW = 40;
constexpr int kH = 8;
constexpr int kTowerReloadTicks = 40; // PROVISIONAL, as the spec row's
constexpr int kTowerRangeTiles = 18;  // PROVISIONAL

struct Block {
    int x{};
    int y{};
};

[[nodiscard]] Block open_block() {
    for (int by = 20; by < 200; by += 2) {
        for (int bx = 20; bx < 200; bx += 2) {
            bool clear = true;
            for (int dy = 0; dy < kH && clear; ++dy) {
                for (int dx = 0; dx < kW && clear; ++dx) {
                    clear = sim::tile_passable(kSeed, bx + dx, by + dy);
                }
            }
            if (clear) {
                return Block{bx, by};
            }
        }
    }
    return Block{-1, -1};
}

struct Spot {
    Side side;
    UnitKind kind;
    int dx;
    int dy;
};

[[nodiscard]] sim::Sim make(const Block& b, const std::vector<Spot>& spots) {
    sim::Scenario scenario{};
    scenario.seed = kSeed;
    for (const Spot& s : spots) {
        sim::Spawn spawn{};
        spawn.side = s.side;
        spawn.kind = s.kind;
        spawn.tile_x = b.x + s.dx;
        spawn.tile_y = b.y + s.dy;
        scenario.spawns.push_back(spawn);
    }
    scenario.store_capacity = 400;
    scenario.start_ore = 400;
    return sim::Sim::create(scenario);
}

[[nodiscard]] const sim::Unit* first_of(const sim::Sim& s, UnitKind kind, Side side) {
    for (const sim::Unit& u : s.units()) {
        if (u.kind == kind && u.side == side) {
            return &u;
        }
    }
    return nullptr;
}

/// Run `ticks`, returning the ticks (1-based) on which `shooter` fired.
[[nodiscard]] std::vector<int> fire_ticks(sim::Sim& s, sim::UnitId shooter, int ticks) {
    std::vector<int> out;
    for (int t = 1; t <= ticks; ++t) {
        s.step();
        for (const sim::Sim::Shot& shot : s.shots()) {
            if (shot.shooter == shooter) {
                out.push_back(t);
            }
        }
    }
    return out;
}

TEST(GunTower, FiresAtAnEnemyInRangeAndHitsIt) {
    const Block b = open_block();
    ASSERT_GE(b.x, 0);
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunTower, 2, 3}, {Side::Enemy, UnitKind::Factory, 12, 3}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    const std::uint16_t before = first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    const std::vector<int> fired = fire_ticks(s, tower, 120);
    EXPECT_FALSE(fired.empty()) << "the tower fired";
    EXPECT_LT(first_of(s, UnitKind::Factory, Side::Enemy)->hp, before) << "and the bolt hit";
}

TEST(GunTower, ItsShotHitsForTheTowersOwnDamage) {
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunTower, 2, 3}, {Side::Enemy, UnitKind::Factory, 8, 3}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    const std::uint16_t before = first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    (void)fire_ticks(s, tower, 80); // two shots at 40 ticks, both landed by then
    const int lost = before - first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    EXPECT_GT(lost, 0);
    EXPECT_EQ(lost % 36, 0) << "every hit is 36 (PROVISIONAL: three times the turret's 12)";
}

TEST(GunTower, DoesNotFireAtAnEnemyBeyondItsRange) {
    const Block b = open_block();
    // 25 tiles east: past the 18-tile reach, and past the bolt's travel too.
    sim::Sim s = make(b, {{Side::Player, UnitKind::GunTower, 2, 3},
                          {Side::Enemy, UnitKind::Factory, 2 + kTowerRangeTiles + 7, 3}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    const std::uint16_t before = first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    EXPECT_TRUE(fire_ticks(s, tower, 200).empty());
    EXPECT_EQ(first_of(s, UnitKind::Factory, Side::Enemy)->hp, before);
}

TEST(GunTower, NeverFiresAtItsOwnSide) {
    const Block b = open_block();
    sim::Sim s = make(b, {{Side::Player, UnitKind::GunTower, 2, 3},
                          {Side::Player, UnitKind::Factory, 8, 3},
                          {Side::Player, UnitKind::Factory, 2, 6}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    EXPECT_TRUE(fire_ticks(s, tower, 400).empty());
    for (const sim::Unit& u : s.units()) {
        EXPECT_EQ(u.hp, sim::spec_of(u.kind).hp) << "nobody on its side was hurt";
    }
}

TEST(GunTower, HoldsFireWhenAFriendStandsInTheLane) {
    const Block b = open_block();
    // A friendly unit squarely between the tower and the enemy: the shot would hit it. (Round 19,
    // L1: the tower is 4x4 now, standing at (4,5) by its centre, so the lane to (12,3) runs through
    // (8,4). Round 19, L2: a factory is 8x8 now and cannot stand four tiles from another, so the
    // two are wheeled RECYCLERS, unarmed and standing on no footprint.)
    sim::Sim s = make(b, {{Side::Player, UnitKind::GunTower, 2, 3},
                          {Side::Player, UnitKind::Recycler, 8, 4},
                          {Side::Enemy, UnitKind::Recycler, 12, 3}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    EXPECT_TRUE(fire_ticks(s, tower, 200).empty());
    const sim::Unit* friendly = first_of(s, UnitKind::Recycler, Side::Player);
    EXPECT_EQ(friendly->hp, sim::spec_of(UnitKind::Recycler).hp);
}

TEST(GunTower, DoesNotMove) {
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunTower, 2, 3}, {Side::Enemy, UnitKind::Factory, 12, 3}});
    const sim::Unit* t = first_of(s, UnitKind::GunTower, Side::Player);
    const sim::UnitId id = t->id;
    const auto x = t->x.raw;
    const auto y = t->y.raw;
    (void)fire_ticks(s, id, 1500);
    const sim::Unit* after = s.find(id);
    ASSERT_NE(after, nullptr);
    EXPECT_EQ(after->x.raw, x);
    EXPECT_EQ(after->y.raw, y);
}

TEST(GunTower, ReloadsOnTheSpecsCooldownToTheTick) {
    EXPECT_EQ(sim::spec_of(UnitKind::GunTower).cooldown, kTowerReloadTicks);
    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::GunTower, 2, 3}, {Side::Enemy, UnitKind::Factory, 8, 3}});
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    const std::vector<int> fired = fire_ticks(s, tower, 300);
    ASSERT_GE(fired.size(), 4U);
    for (std::size_t i = 1; i < fired.size(); ++i) {
        EXPECT_EQ(fired[i] - fired[i - 1], kTowerReloadTicks) << "shot " << i;
    }
}

TEST(GunTower, ADestroyedTowerFiresNothing) {
    const Block b = open_block();
    // Six enemy tanks well inside their own reach of the tower kill it before it kills them all.
    std::vector<Spot> spots{{Side::Player, UnitKind::GunTower, 2, 3},
                            {Side::Enemy, UnitKind::Factory, 14, 3}};
    for (int i = 0; i < 8; ++i) {
        spots.push_back({Side::Enemy, UnitKind::Tank, 9, i});
    }
    sim::Sim s = make(b, spots);
    const sim::UnitId tower = first_of(s, UnitKind::GunTower, Side::Player)->id;
    int died = -1;
    for (int t = 1; t <= 400 && died < 0; ++t) {
        s.step();
        if (!s.find(tower)->alive) {
            died = t;
        }
    }
    ASSERT_GT(died, 0) << "the tower can be shot down like any building";
    const std::uint16_t factory_hp = first_of(s, UnitKind::Factory, Side::Enemy)->hp;
    EXPECT_TRUE(fire_ticks(s, tower, 300).empty());
    // (Nothing of the tower's is still in the air to land: the factory is only ever hurt by it.)
    EXPECT_EQ(first_of(s, UnitKind::Factory, Side::Enemy)->hp, factory_hp);
}

TEST(GunTower, IsABuildingAConstructorRaisesAndNoProducerQueues) {
    EXPECT_FALSE(sim::is_queueable(UnitKind::GunTower));
    EXPECT_TRUE(sim::is_placeable(UnitKind::GunTower));
    EXPECT_FALSE(sim::spec_of(UnitKind::GunTower).buildable);
    EXPECT_EQ(sim::spec_of(UnitKind::GunTower).max_speed.raw, 0);
    EXPECT_EQ(static_cast<int>(sim::spec_of(UnitKind::GunTower).cost), 50);

    const Block b = open_block();
    sim::Sim s = make(
        b, {{Side::Player, UnitKind::Constructor, 4, 3}, {Side::Enemy, UnitKind::Factory, 12, 3}});
    const sim::UnitId constructor = first_of(s, UnitKind::Constructor, Side::Player)->id;
    s.enqueue(
        sim::Order{constructor, sim::orders::Place{UnitKind::GunTower, b.x + 5, b.y + 3, false}});
    s.step();
    const sim::Unit* site = first_of(s, UnitKind::GunTower, Side::Player);
    ASSERT_NE(site, nullptr);
    ASSERT_TRUE(sim::under_construction(*site));
    const sim::UnitId id = site->id;
    // While it is a SITE it is not a gun: not one shot until the build job has finished.
    int built = -1;
    for (int t = 1; t <= 1000 + 600 && built < 0; ++t) {
        s.step();
        for (const sim::Sim::Shot& shot : s.shots()) {
            if (shot.shooter == id) {
                EXPECT_FALSE(sim::under_construction(*s.find(id))) << "a site fired";
            }
        }
        if (!sim::under_construction(*s.find(id))) {
            built = t;
        }
    }
    ASSERT_GT(built, 0) << "the Constructor built it";
    EXPECT_FALSE(fire_ticks(s, id, 200).empty()) << "and the finished tower shoots";
}

} // namespace
} // namespace protekt
