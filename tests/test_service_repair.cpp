#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/// SERVICE TRUCKS (D1, the sim half). A truck mends, on its own, whatever friendly unit is within
/// `kServiceRepairRadiusTiles` and hurt: `max_hp / kServiceRepairTicks` a tick at the closest, so
/// a tank goes from nearly dead to whole in about 20 s and a thing with twice the hit points takes
/// twice as long. Closer is faster; every truck in range adds its own share. (The trucks face
/// the patient throughout: what is ahead of a truck is `test_repair_cone.cpp`'s.)
///
/// The ground is asserted open rather than described, and the enemy base is unarmed and far away:
/// nothing here is shot, so every hit point gained is the trucks'.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kRowY = 22;
constexpr int kPatientX = 110;
constexpr int kFirstX = 102;
constexpr int kLastX = 124;
constexpr int kSoak
    = 300; ///< ticks: half of a full repair, so a rate is read well clear of its cap

[[nodiscard]] bool open_row() {
    for (int y = kRowY - 3; y <= kRowY + 3; ++y) {
        for (int x = kFirstX; x <= kLastX; ++x) {
            if (world::sample(kSeed, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, std::uint16_t hp = 0) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = kRowY;
    s.hp = hp;
    return s;
}

[[nodiscard]] Sim rig(std::vector<Spawn> spawns) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = std::move(spawns);
    Spawn base{};
    base.kind = UnitKind::Factory;
    base.side = Side::Enemy;
    base.tile_x = 200;
    base.tile_y = 200;
    scenario.spawns.push_back(base);
    return Sim::create(scenario);
}

void run(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

/// A wounded `kind` at the patient tile with trucks at the given tile columns, as the sim placed
/// them. The patient is unit 0; the trucks follow.
[[nodiscard]] Sim ward(UnitKind kind, std::uint16_t hp, const std::vector<int>& truck_columns) {
    std::vector<Spawn> spawns{spawn(kind, Side::Player, kPatientX, hp)};
    for (const int column : truck_columns) {
        // Every truck faces the patient: these tests are about the DISTANCE rule, so the shotgun
        // cone (`RepairCone.*`) is held at its centre and takes nothing off.
        Spawn truck = spawn(UnitKind::ServiceTruck, Side::Player, column);
        truck.face_x = column < kPatientX ? 1 : -1;
        spawns.push_back(truck);
    }
    return rig(std::move(spawns));
}

/// Hit points the patient (unit 0) gains in `ticks` from trucks at `truck_columns`.
[[nodiscard]] int gained(UnitKind kind, const std::vector<int>& truck_columns, int ticks = kSoak) {
    Sim sim = ward(kind, 1, truck_columns);
    run(sim, ticks);
    return static_cast<int>(sim.units().at(0).hp) - 1;
}

/// Whole tiles between the patient and the first truck, asserted exact so a test that names a
/// distance really tests it.
[[nodiscard]] int tiles_apart(const Sim& sim) {
    const Unit& a = sim.units().at(0);
    const Unit& b = sim.units().at(1);
    EXPECT_EQ(a.y.raw, b.y.raw) << "the pair must stand on one row";
    return (b.x.raw - a.x.raw) / Fixed::kOne;
}

/// Whether the first truck stands within reach of the patient, by true distance: a building's
/// footprint can put its centre off the row it was asked for.
[[nodiscard]] bool truck_in_reach(const Sim& sim) {
    const std::int64_t dx = sim.units().at(1).x.raw - sim.units().at(0).x.raw;
    const std::int64_t dy = sim.units().at(1).y.raw - sim.units().at(0).y.raw;
    const std::int64_t radius = static_cast<std::int64_t>(kServiceRepairRadiusTiles) * Fixed::kOne;
    return dx * dx + dy * dy <= radius * radius;
}

TEST(ServiceRepair, TheGroundIsOpen) { ASSERT_TRUE(open_row()); }

TEST(ServiceRepair, ATankFromNearlyDeadToWholeTakesAboutTwentySeconds) {
    Sim sim = ward(UnitKind::Tank, 1, {kPatientX + 1});
    int ticks = 0;
    while (sim.units().at(0).hp < kTankHp && ticks < 5 * kServiceRepairTicks) {
        sim.step();
        ++ticks;
    }
    EXPECT_EQ(sim.units().at(0).hp, kTankHp);
    EXPECT_GE(ticks, 20 * kTicksPerSecond * 9 / 10);
    EXPECT_LE(ticks, 20 * kTicksPerSecond * 11 / 10);
}

TEST(ServiceRepair, TheMendingIsSlowNotAJump) {
    Sim sim = ward(UnitKind::Tank, 1, {kPatientX + 1});
    run(sim, kSoak);
    EXPECT_GT(sim.units().at(0).hp, 40);
    EXPECT_LT(sim.units().at(0).hp, 60);
}

TEST(ServiceRepair, TimeScalesWithMaxHealth) {
    // Same truck, same distance, three different hulls: what each gains is its own max_hp's share
    // of the same fraction, so twice the hit points is twice the time. (A hit point is whole, so
    // allow the one the rounding can cost.)
    const int tank = gained(UnitKind::Tank, {kPatientX + 3});
    const int turret = gained(UnitKind::Turret, {kPatientX + 3});
    const int bomber = gained(UnitKind::Bomber, {kPatientX + 3});
    ASSERT_GT(tank, 20);
    EXPECT_NEAR(turret * kTankHp, tank * kTurretHp, kTurretHp);
    EXPECT_NEAR(bomber * kTankHp, tank * spec_of(UnitKind::Bomber).hp, kTankHp);
}

TEST(ServiceRepair, AFactoryTakesFourTimesAsLongAsATank) {
    // 400 against 100: the buildings are not special-cased, they simply have more to mend.
    const int tank = gained(UnitKind::Tank, {kPatientX + 3});
    Sim sim = ward(UnitKind::Factory, 1, {kPatientX + 3});
    ASSERT_TRUE(truck_in_reach(sim));
    run(sim, kSoak);
    const int factory = static_cast<int>(sim.units().at(0).hp) - 1;
    EXPECT_GT(factory, 0);
    // The factory's centre is a little off its spawn tile, so the truck is a little nearer or
    // farther than 3 tiles: compare the rate per hit point, loosely.
    EXPECT_GT(factory * kTankHp, tank * kFactoryHp / 2);
    EXPECT_LT(factory * kTankHp, tank * kFactoryHp * 2);
}

TEST(ServiceRepair, OutsideSixTilesNothingHappens) {
    Sim edge = ward(UnitKind::Tank, 1, {kPatientX + kServiceRepairRadiusTiles});
    ASSERT_EQ(tiles_apart(edge), kServiceRepairRadiusTiles);
    run(edge, kSoak);
    EXPECT_GT(edge.units().at(0).hp, 1) << "exactly on the radius is inside it";

    Sim out = ward(UnitKind::Tank, 1, {kPatientX + kServiceRepairRadiusTiles + 1});
    ASSERT_EQ(tiles_apart(out), kServiceRepairRadiusTiles + 1);
    run(out, kSoak);
    EXPECT_EQ(out.units().at(0).hp, 1);
}

TEST(ServiceRepair, TheCloserTheTruckTheFasterTheMending) {
    const int near = gained(UnitKind::Tank, {kPatientX + 1});
    const int mid = gained(UnitKind::Tank, {kPatientX + 3});
    const int far = gained(UnitKind::Tank, {kPatientX + 5});
    const int edge = gained(UnitKind::Tank, {kPatientX + kServiceRepairRadiusTiles});
    EXPECT_GT(near, mid);
    EXPECT_GT(mid, far);
    EXPECT_GT(far, edge);
    EXPECT_GT(edge, 0) << "slow at the edge, but not stopped";
}

TEST(ServiceRepair, MoreTrucksMendFaster) {
    // Trucks at equal distances either side, so the only difference is how many there are.
    const int one = gained(UnitKind::Tank, {kPatientX + 3});
    const int two = gained(UnitKind::Tank, {kPatientX + 3, kPatientX - 3});
    const int three = gained(UnitKind::Tank, {kPatientX + 3, kPatientX - 3, kPatientX + 2});
    EXPECT_GT(two, one);
    EXPECT_GT(three, two);
    EXPECT_NEAR(two, 2 * one, 2) << "each truck adds its own share";
}

TEST(ServiceRepair, EveryFriendlyKindIsMended) {
    // One rule, not a list: whatever the kind, hurt and within reach it gains. The patient is a
    // quarter of its hit points (at least 1) short of whole. A second truck stands by so a
    // patient that is itself a truck still has someone to mend it.
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        const std::uint16_t max_hp = spec_of(kind).hp;
        Sim sim = ward(kind, 1, {kPatientX + 3, kPatientX - 3});
        ASSERT_GE(max_hp, 2) << spec_of(kind).name;
        ASSERT_TRUE(truck_in_reach(sim)) << spec_of(kind).name;
        run(sim, kSoak);
        EXPECT_GT(sim.units().at(0).hp, 1) << spec_of(kind).name << " was not mended";
        EXPECT_LE(sim.units().at(0).hp, max_hp) << spec_of(kind).name;
    }
}

TEST(ServiceRepair, TheDeployedFixturesAreMendedToo) {
    // Named, because the brief does: the deployed turret and the deployed scavenger are kinds of
    // their own, and the loop above is the test that none was left out.
    EXPECT_GT(gained(UnitKind::DeployedTurret, {kPatientX + 3}), 0);
    EXPECT_GT(gained(UnitKind::DeployedScavenger, {kPatientX + 3}), 0);
    EXPECT_GT(gained(UnitKind::Turret, {kPatientX + 3}), 0);
}

TEST(ServiceRepair, NeverPastTheMaximum) {
    Sim sim = ward(UnitKind::Tank, kTankHp - 2, {kPatientX + 1, kPatientX - 1, kPatientX + 2});
    run(sim, 2 * kServiceRepairTicks);
    EXPECT_EQ(sim.units().at(0).hp, kTankHp);
}

TEST(ServiceRepair, ATruckDoesNotMendTheEnemyOrItself) {
    Sim enemy = rig({spawn(UnitKind::Tank, Side::Enemy, kPatientX, 1),
                     spawn(UnitKind::ServiceTruck, Side::Player, kPatientX + 2)});
    run(enemy, kSoak);
    EXPECT_EQ(enemy.units().at(0).hp, 1);

    Sim lone = rig({spawn(UnitKind::ServiceTruck, Side::Player, kPatientX, 1)});
    run(lone, kSoak);
    EXPECT_EQ(lone.units().at(0).hp, 1);
}

TEST(ServiceRepair, OnlyTheTruckMends) {
    // A pod, a tug and a constructor are support hulls too, but the repair is the truck's.
    for (const UnitKind kind : {UnitKind::ServicePod, UnitKind::Tug, UnitKind::Constructor}) {
        Sim sim = rig({spawn(UnitKind::Tank, Side::Player, kPatientX, 1),
                       spawn(kind, Side::Player, kPatientX + 2)});
        run(sim, kSoak);
        EXPECT_EQ(sim.units().at(0).hp, 1) << spec_of(kind).name;
    }
}

} // namespace
} // namespace protekt::sim
