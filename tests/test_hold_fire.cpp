#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

/// EVERYBODY SHOULD TRY TO AVOID FRIENDLY FIRE (Harri, after his turrets killed one another round
/// an enemy extractor). A gun that has a target holds fire when its shot would hurt a friend; it
/// does not disable firing, and the damage a friendly takes from a round that does reach it is not
/// touched. The targets here are RECYCLERS (unarmed), so every hp change is the gun under test's.
/// (A RECYCLER stands in for the factory these once used: it is unarmed and idle too, and stands on
/// no footprint -- a factory is 8 x 8 since round 19, L2, so two of them four tiles apart cannot
/// exist.)
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

struct Place {
    Side side{};
    UnitKind kind{};
    int tile_x{};
    int tile_y{};
};

[[nodiscard]] Sim make(const std::vector<Place>& places) {
    Scenario scenario{};
    scenario.seed = 42;
    for (const Place& place : places) {
        Spawn spawn{};
        spawn.side = place.side;
        spawn.kind = place.kind;
        spawn.tile_x = place.tile_x;
        spawn.tile_y = place.tile_y;
        scenario.spawns.push_back(spawn);
    }
    return Sim::create(scenario);
}

void run(Sim& sim, int ticks) {
    for (int tick = 0; tick < ticks; ++tick) {
        sim.step();
    }
}

std::uint16_t hp(const Sim& sim, std::uint32_t id) { return sim.find(UnitId{id})->hp; }
std::uint16_t full(const Sim& sim, std::uint32_t id) {
    return profile(sim.find(UnitId{id})->kind).hp;
}

// Ids are spawn order from 1: the gun, the friend, the enemy.
constexpr std::uint32_t kGun = 1;
constexpr std::uint32_t kFriend = 2;
constexpr std::uint32_t kEnemy = 3;

TEST(HoldFire, ADirectGunHoldsFireWhenAFriendStandsInItsLane) {
    // The friend is four tiles out on the line to the enemy at eight: the bolt would stop on it.
    Sim sim = make({{Side::Player, UnitKind::Tank, 100, 100},
                    {Side::Player, UnitKind::Recycler, 104, 100},
                    {Side::Enemy, UnitKind::Recycler, 108, 100}});
    run(sim, 150);
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend)) << "it shot through its own side";
    EXPECT_EQ(hp(sim, kEnemy), full(sim, kEnemy)) << "the friend shields it, so it was not shot";
    EXPECT_EQ(sim.find(UnitId{kGun})->cooldown, 0) << "it never took the shot";
}

TEST(HoldFire, ADirectGunWithAClearLaneStillFires) {
    // The same friend, three tiles off the line: out of the lane, so the enemy is shot.
    Sim sim = make({{Side::Player, UnitKind::Tank, 100, 100},
                    {Side::Player, UnitKind::Recycler, 104, 103},
                    {Side::Enemy, UnitKind::Recycler, 108, 100}});
    run(sim, 150);
    EXPECT_LT(hp(sim, kEnemy), full(sim, kEnemy));
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend));
}

TEST(HoldFire, AGunPassesOverABlockedTargetForOneItCanShoot) {
    // The near enemy is behind the friend; a second enemy, farther but clear, is shot instead of
    // the gun standing idle: it TRIES not to hurt a friend, it does not stop fighting.
    Sim sim = make({{Side::Player, UnitKind::Tank, 100, 100},
                    {Side::Player, UnitKind::Recycler, 104, 100},
                    {Side::Enemy, UnitKind::Recycler, 106, 100},
                    {Side::Enemy, UnitKind::Recycler, 100, 109}});
    run(sim, 150);
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend));
    EXPECT_EQ(hp(sim, 3), full(sim, 3));
    EXPECT_LT(hp(sim, 4), full(sim, 4));
}

TEST(HoldFire, ARoundThatDoesHitAFriendlyStillDamagesIt) {
    // The damage was not touched: the player's trigger takes the very shot the gun declines, and
    // the friend in the lane is hurt by it.
    // Factories again (a flat bolt strikes a factory's tall body), 8 x 8 since round 19, L2: they
    // stand centred five and thirteen tiles from the tank, on a level row.
    const RowAt row = level_row(14);
    ASSERT_GE(row.x, 0);
    Sim sim = make({{Side::Player, UnitKind::Tank, row.x, row.y},
                    {Side::Player, UnitKind::Factory, row.x + 1, row.y - 4},
                    {Side::Enemy, UnitKind::Factory, row.x + 9, row.y - 4}});
    run(sim, profile(UnitKind::Tank).cooldown_ticks);
    sim.enqueue(Order{UnitId{kGun},
                      orders::FireTowards{Fixed::from_int(1), Fixed::zero(), UnitId{kEnemy}}});
    run(sim, 60);
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend) - profile(UnitKind::Tank).damage);
    EXPECT_EQ(hp(sim, kEnemy), full(sim, kEnemy));
}

TEST(HoldFire, ALobbedShellGoesOverAFriendUnderItsArc) {
    // The friend is four tiles out on the straight line to the enemy at eleven. A straight-lane
    // test would refuse; the shell goes OVER it and comes down on the enemy, so the mortar fires.
    Sim sim = make({{Side::Player, UnitKind::MortarBike, 100, 100},
                    {Side::Player, UnitKind::Recycler, 104, 100},
                    {Side::Enemy, UnitKind::Recycler, 111, 100}});
    run(sim, 200);
    EXPECT_LT(hp(sim, kEnemy), full(sim, kEnemy)) << "the mortar refused a shot that was clear";
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend));
}

TEST(HoldFire, ALobbedShellHoldsWhenAFriendStandsInItsSplash) {
    // The friend is a tile from the enemy, inside the 1.5-tile splash: the shell would burst on it.
    Sim sim = make({{Side::Player, UnitKind::MortarBike, 100, 100},
                    {Side::Player, UnitKind::Recycler, 111, 101},
                    {Side::Enemy, UnitKind::Recycler, 111, 100}});
    run(sim, 200);
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend));
    EXPECT_EQ(hp(sim, kEnemy), full(sim, kEnemy));
}

TEST(HoldFire, ALobbedShellWithAClearLandingFires) {
    // The same friend three tiles from the enemy: outside the splash, so the shell is taken.
    Sim sim = make({{Side::Player, UnitKind::MortarBike, 100, 100},
                    {Side::Player, UnitKind::Recycler, 111, 103},
                    {Side::Enemy, UnitKind::Recycler, 111, 100}});
    run(sim, 200);
    EXPECT_LT(hp(sim, kEnemy), full(sim, kEnemy));
    EXPECT_EQ(hp(sim, kFriend), full(sim, kFriend));
}

} // namespace
} // namespace protekt::sim
