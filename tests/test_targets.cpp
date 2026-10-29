// BZ2A-1: the attack list. The nearest living hostiles of a side within the radar's radius, nearest
// first by squared Q16.16 distance, ties to the lower id, ten at most, keyed 1-9 then 0.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "protekt/game/radar.hpp"
#include "protekt/game/targets.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitId;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;

/// Ids follow spawn order from 1 (the sim numbers them as it creates them); the tests pin that.
[[nodiscard]] UnitId id_of(int n) { return UnitId{static_cast<std::uint32_t>(n)}; }

[[nodiscard]] sim::Sim make(const std::vector<Spawn>& spawns) {
    return sim::Sim::create(sim::Scenario{kSeed, spawns});
}

[[nodiscard]] std::vector<UnitId> ids_of(const std::vector<TargetRow>& rows) {
    std::vector<UnitId> ids;
    ids.reserve(rows.size());
    for (const TargetRow& row : rows) {
        ids.push_back(row.id);
    }
    return ids;
}

TEST(Targets, NearestFirstWithKeysOneToN) {
    // Our tank is unit 1 at (50,50). Hostiles, in spawn order: 2 at 10 tiles, 3 at 3, 4 at 25
    // (a 7-24 triangle), 5 at 6.
    sim::Sim sim = make({{UnitKind::Tank, Side::Player, 50, 50},
                         {UnitKind::Tank, Side::Enemy, 60, 50},
                         {UnitKind::Turret, Side::Enemy, 50, 53},
                         {UnitKind::MortarBike, Side::Enemy, 57, 74},
                         {UnitKind::RocketTank, Side::Enemy, 44, 50}});
    ASSERT_EQ(sim.units().at(0).id, id_of(1));
    const std::vector<TargetRow> rows
        = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(rows.size(), 4U);
    EXPECT_EQ(ids_of(rows), (std::vector<UnitId>{id_of(3), id_of(5), id_of(2), id_of(4)}));
    EXPECT_EQ(rows.at(0).key, 1);
    EXPECT_EQ(rows.at(1).key, 2);
    EXPECT_EQ(rows.at(2).key, 3);
    EXPECT_EQ(rows.at(3).key, 4);
    EXPECT_EQ(rows.at(0).kind, UnitKind::Turret);
    EXPECT_EQ(rows.at(3).kind, UnitKind::MortarBike);
    EXPECT_EQ(rows.at(0).metres, 6);
    EXPECT_EQ(rows.at(1).metres, 12);
    EXPECT_EQ(rows.at(2).metres, 20);
    EXPECT_EQ(rows.at(3).metres, 50);
}

TEST(Targets, ExactlyAtTheRadiusIsInAndOneTileFurtherIsOut) {
    sim::Sim sim = make({{UnitKind::Tank, Side::Player, 10, 10},
                         {UnitKind::Tank, Side::Enemy, 10 + kRadarRadiusTiles, 10},
                         {UnitKind::Tank, Side::Enemy, 10, 10 + kRadarRadiusTiles + 1}});
    const std::vector<TargetRow> rows
        = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.at(0).id, id_of(2));
    EXPECT_EQ(rows.at(0).metres, 80);
}

TEST(Targets, OwnUnitsNeverAppearAndTheDeadDisappear) {
    // Unit 1 is the centre: a factory, which is unarmed and takes a while to kill. Two of our
    // turrets (2, 3) stand on the lone enemy tank (4) and will kill it; the enemy factory (5)
    // is unarmed and a long way from dead.
    sim::Sim sim = make({{UnitKind::Factory, Side::Player, 30, 41},
                         {UnitKind::Turret, Side::Player, 44, 41},
                         {UnitKind::Turret, Side::Player, 44, 42},
                         {UnitKind::Tank, Side::Enemy, 46, 41},
                         {UnitKind::Factory, Side::Enemy, 34, 50}});
    std::vector<TargetRow> rows = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    EXPECT_EQ(ids_of(rows), (std::vector<UnitId>{id_of(5), id_of(4)}))
        << "our own turrets are never listed";
    for (int tick = 0; tick < 12000 && sim.find(id_of(4))->alive; ++tick) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(id_of(4))->alive) << "the tank outlived the test, so nothing is proved";
    ASSERT_TRUE(sim.find(id_of(1))->alive);
    rows = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(rows.size(), 1U);
    EXPECT_EQ(rows.at(0).id, id_of(5));
    EXPECT_EQ(rows.at(0).key, 1) << "the survivors close up";
}

TEST(Targets, EqualDistanceGoesToTheLowerIdWhateverTheSpawnOrder) {
    // Units 2 and 3 are both 5 tiles out; the other order spawns them the other way round. Their
    // KINDS differ so the row says which id it is.
    sim::Sim a = make({{UnitKind::Tank, Side::Player, 50, 50},
                       {UnitKind::Turret, Side::Enemy, 55, 50},
                       {UnitKind::MortarBike, Side::Enemy, 50, 45}});
    sim::Sim b = make({{UnitKind::Tank, Side::Player, 50, 50},
                       {UnitKind::MortarBike, Side::Enemy, 50, 45},
                       {UnitKind::Turret, Side::Enemy, 55, 50}});
    const std::vector<TargetRow> ra = nearest_enemies(a, Side::Player, id_of(1), kRadarRadiusTiles);
    const std::vector<TargetRow> rb = nearest_enemies(b, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(ra.size(), 2U);
    ASSERT_EQ(rb.size(), 2U);
    EXPECT_EQ(ra.at(0).id, id_of(2));
    EXPECT_EQ(ra.at(0).kind, UnitKind::Turret);
    EXPECT_EQ(rb.at(0).id, id_of(2));
    EXPECT_EQ(rb.at(0).kind, UnitKind::MortarBike);
    EXPECT_EQ(ra.at(1).id, id_of(3));
    EXPECT_EQ(rb.at(1).id, id_of(3));
}

TEST(Targets, MoreThanTenIsExactlyTenAndTheTenthIsKeyedZero) {
    // Twelve hostiles at 2, 4 ... 24 tiles due east, spawned FARTHEST first so spawn order is no
    // help.
    std::vector<Spawn> spawns{{UnitKind::Tank, Side::Player, 20, 50}};
    for (int d = 24; d >= 2; d -= 2) {
        spawns.push_back({UnitKind::Tank, Side::Enemy, 20 + d, 50});
    }
    sim::Sim sim = make(spawns);
    const std::vector<TargetRow> rows
        = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(rows.size(), kAttackListRows);
    EXPECT_EQ(rows.at(0).key, 1);
    EXPECT_EQ(rows.at(8).key, 9);
    EXPECT_EQ(rows.at(9).key, 0);
    // The d-tile hostile is unit 14 - d/2 (d = 24 is unit 2). The nearest is unit 13; the tenth,
    // at 20 tiles, is unit 4.
    EXPECT_EQ(rows.at(0).id, id_of(13));
    EXPECT_EQ(rows.at(9).id, id_of(4));
    EXPECT_EQ(rows.at(9).metres, 40);
}

TEST(Targets, ACentreThatIsNotALivingUnitOfTheAskingSideGivesNoList) {
    // Unit 1 is a lone turret in the middle of three enemy tanks (2-4), and does not survive.
    sim::Sim sim = make({{UnitKind::Turret, Side::Player, 44, 41},
                         {UnitKind::Tank, Side::Enemy, 45, 41},
                         {UnitKind::Tank, Side::Enemy, 46, 42},
                         {UnitKind::Tank, Side::Enemy, 45, 40}});
    ASSERT_EQ(nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles).size(), 3U);
    EXPECT_TRUE(nearest_enemies(sim, Side::Player, id_of(2), kRadarRadiusTiles).empty())
        << "the enemy's unit is nothing to measure from";
    EXPECT_TRUE(nearest_enemies(sim, Side::Player, UnitId{}, kRadarRadiusTiles).empty());
    EXPECT_TRUE(nearest_enemies(sim, Side::Player, id_of(99), kRadarRadiusTiles).empty());
    for (int tick = 0; tick < 8000 && sim.find(id_of(1))->alive; ++tick) {
        sim.step();
    }
    ASSERT_FALSE(sim.find(id_of(1))->alive) << "the turret outlived the test, so nothing is proved";
    EXPECT_TRUE(nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles).empty())
        << "a dead centre is not the camera, the map centre or the origin";
}

TEST(Targets, MetresAreTilesTimesTwoRounded) {
    sim::Sim sim = make({{UnitKind::Tank, Side::Player, 10, 10},
                         {UnitKind::Tank, Side::Enemy, 40, 10},
                         {UnitKind::Tank, Side::Enemy, 11, 11}});
    const std::vector<TargetRow> rows
        = nearest_enemies(sim, Side::Player, id_of(1), kRadarRadiusTiles);
    ASSERT_EQ(rows.size(), 2U);
    EXPECT_EQ(rows.at(0).metres, 3) << "sqrt(2) tiles = 2.83 m, rounded";
    EXPECT_EQ(rows.at(1).metres, 60) << "30 tiles";
}

TEST(Targets, TheEnemyAsksAndGetsTheirsBack) {
    sim::Sim sim = make({{UnitKind::Tank, Side::Player, 50, 50},
                         {UnitKind::Turret, Side::Player, 55, 50},
                         {UnitKind::Tank, Side::Enemy, 52, 50}});
    const std::vector<TargetRow> rows
        = nearest_enemies(sim, Side::Enemy, id_of(3), kRadarRadiusTiles);
    EXPECT_EQ(ids_of(rows), (std::vector<UnitId>{id_of(1), id_of(2)}));
    EXPECT_EQ(rows.at(0).metres, 4);
    EXPECT_TRUE(nearest_enemies(sim, Side::Enemy, id_of(1), kRadarRadiusTiles).empty());
}

} // namespace
} // namespace protekt::game
