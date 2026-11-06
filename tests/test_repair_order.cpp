// THE REPAIR ORDER (D1b, repair by TARGETING). Harri, 2026-11-05: "maybe the same logic that's used
// to attack an enemy can be used to repair a friendly." A truck that is SENT to a damaged friend
// drives to it and mends it; the automatic mending of D1 is untouched and is what does the work
// once the truck is in reach. Follow could not carry the errand: it never ends, so a truck sent to
// a tank would trail it for ever; a Repair lapses to Idle when the patient is whole or gone.
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kRowY = 22;
constexpr int kPatientX = 102;
constexpr int kTruckX = 124; ///< 22 tiles away: far outside the mending radius
constexpr std::uint16_t kWounded = 1;

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, std::uint16_t hp = 0, int y = kRowY) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    s.hp = hp;
    return s;
}

/// Unit 0 a wounded friendly tank, unit 1 a truck of the same side far off, unit 2 a friendly
/// scout, unit 3 a wounded enemy tank well out of gun range of all of them, and an unarmed enemy
/// factory a long way away.
[[nodiscard]] Sim rig() {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(UnitKind::Tank, Side::Player, kPatientX, kWounded),
                       spawn(UnitKind::ServiceTruck, Side::Player, kTruckX),
                       spawn(UnitKind::Scout, Side::Player, kPatientX + 4),
                       spawn(UnitKind::Tank, Side::Enemy, 180, kWounded, 180)};
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 0, 200));
    return Sim::create(scenario);
}

void run(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

[[nodiscard]] Order repair(const Sim& sim, std::size_t truck, std::size_t patient) {
    return Order{sim.units().at(truck).id, orders::Repair{sim.units().at(patient).id}};
}

[[nodiscard]] std::int64_t tiles_between(const Sim& sim, std::size_t a, std::size_t b) {
    const std::int64_t dx = sim.units().at(a).x.raw - sim.units().at(b).x.raw;
    const std::int64_t dy = sim.units().at(a).y.raw - sim.units().at(b).y.raw;
    return (dx * dx + dy * dy) / (static_cast<std::int64_t>(Fixed::kOne) * Fixed::kOne);
}

TEST(RepairOrder, TheGroundIsOpen) {
    for (int y = kRowY - 3; y <= kRowY + 3; ++y) {
        for (int x = kPatientX; x <= kTruckX; ++x) {
            ASSERT_EQ(world::sample(kSeed, x, y).terrain, world::Terrain::Flat);
        }
    }
}

TEST(RepairOrder, WithoutTheOrderAFarTruckNeverMendsAnything) {
    Sim sim = rig();
    run(sim, 600);
    EXPECT_EQ(sim.units().at(0).hp, kWounded) << "the control: the truck is out of reach";
    EXPECT_EQ(sim.units().at(1).x.raw, 124 * Fixed::kOne) << "and it stays where it was put";
}

TEST(RepairOrder, ATruckSentToAPatientDrivesToItAndMendsItToFull) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 0));
    run(sim, 1);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Repair);
    EXPECT_EQ(sim.units().at(1).target, sim.units().at(0).id);

    int ticks = 1;
    while (sim.units().at(0).hp < spec_of(UnitKind::Tank).hp && ticks < 3000) {
        sim.step();
        ++ticks;
    }
    EXPECT_EQ(sim.units().at(0).hp, spec_of(UnitKind::Tank).hp) << "the patient was mended";
    const std::int64_t reach = kServiceRepairRadiusTiles;
    EXPECT_LE(tiles_between(sim, 1, 0), reach * reach) << "by a truck that drove into reach";
}

TEST(RepairOrder, TheTruckWentBecauseItWasSentAndStopsAtTheApproachRadius) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 0));
    run(sim, 400);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Repair) << "the patient is not whole yet";
    EXPECT_LE(tiles_between(sim, 1, 0),
              static_cast<std::int64_t>(kServiceRepairApproachTiles) * kServiceRepairApproachTiles)
        << "it closes to the approach radius, nearer than the reach it needs";
    EXPECT_GT(sim.units().at(0).hp, kWounded);
}

TEST(RepairOrder, TheOrderLapsesToIdleWhenThePatientIsWhole) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 0));
    run(sim, 3000);
    ASSERT_EQ(sim.units().at(0).hp, spec_of(UnitKind::Tank).hp);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle);
    EXPECT_FALSE(sim.units().at(1).target.valid());
}

TEST(RepairOrder, TheOrderLapsesWhenThePatientIsDestroyed) {
    // The patient has one hit point and an enemy tank beside it: it dies in the first volleys,
    // long before the truck, 22 tiles off, could reach it.
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = {spawn(UnitKind::Tank, Side::Player, kPatientX, kWounded),
                       spawn(UnitKind::ServiceTruck, Side::Player, kTruckX),
                       spawn(UnitKind::Tank, Side::Enemy, kPatientX + 4),
                       spawn(UnitKind::Factory, Side::Enemy, 200, 0, 200)};
    Sim sim = Sim::create(scenario);
    sim.enqueue(repair(sim, 1, 0));
    run(sim, 1);
    ASSERT_TRUE(sim.units().at(0).alive) << "the patient is still there when the order lands";
    ASSERT_EQ(sim.units().at(1).stance, Stance::Repair) << "so the order was taken, not refused";
    run(sim, 299);
    ASSERT_FALSE(sim.units().at(0).alive) << "the patient was shot";
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle) << "no patient, no errand";
    EXPECT_FALSE(sim.units().at(1).target.valid());
}

TEST(RepairOrder, OnlyAServiceTruckTakesIt) {
    Sim sim = rig();
    sim.enqueue(Order{sim.units().at(2).id, orders::Repair{sim.units().at(0).id}}); // a scout
    run(sim, 1);
    EXPECT_EQ(sim.units().at(2).stance, Stance::Idle) << "a scout has nothing to mend with";
}

TEST(RepairOrder, ItIsRefusedForAnEnemyPatient) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 3));
    run(sim, 1);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle) << "a truck mends its own side";
}

TEST(RepairOrder, ItIsRefusedForATruckNamingItself) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 1));
    run(sim, 1);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle);
}

TEST(RepairOrder, AnotherOrderReplacesIt) {
    Sim sim = rig();
    sim.enqueue(repair(sim, 1, 0));
    run(sim, 5);
    ASSERT_EQ(sim.units().at(1).stance, Stance::Repair);
    sim.enqueue(Order{sim.units().at(1).id, orders::Stop{}});
    run(sim, 1);
    EXPECT_EQ(sim.units().at(1).stance, Stance::Idle);
}

TEST(RepairOrder, ItCarriesATargetAndNothingElse) {
    const OrderFields f = fields_of(orders::Repair{UnitId{7}});
    EXPECT_EQ(f.position_count, 0U);
    EXPECT_TRUE(f.has_target);
    EXPECT_EQ(f.target, UnitId{7});
    EXPECT_EQ(kind_of(OrderPayload{orders::Repair{}}), OrderKind::Repair);
    EXPECT_EQ(static_cast<int>(OrderKind::Repair), 19) << "appended after CancelBuild";
}

} // namespace
} // namespace protekt::sim
