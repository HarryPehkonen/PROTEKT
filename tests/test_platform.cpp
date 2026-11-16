#include "protekt/sim/orders.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

/// THE DRIVE-ON PLATFORM (N1): the SERVICE BAY and its alien twin the DOWER are ONE mechanism. The
/// owner's report: "I can't seem to be able to drive onto the platform on the Service Bay. If I'm
/// in its vicinity, nothing gets repaired. The health bar stays where it is. I should be able to
/// drive onto the platform." So the building's footprint does not block units (it is a platform),
/// and a unit of its own side whose centre stands inside it is mended at the SERVICE TRUCK's own
/// rate (`service_repair_weight` at its full, `kServiceRepairFull`) -- no second repair, no second
/// rate. A unit merely NEAR is not mended. The ammunition half of the Service Bay is NOT BUILT:
/// PROTEKT's weapons have cooldowns, not rounds, so there is no ammunition to refill.
///
/// Every test is run for BOTH kinds (the `TEST_P` below), and one test pins that an ordinary
/// building -- the Power plant -- still blocks the very same move.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kRowY = 30;
constexpr int kBayX = 68; ///< the platform's north-west tile: it covers 68..75 x 26..33
constexpr int kBayY = kRowY - 4;
constexpr int kApproachX = 77; ///< one tile east of the platform's edge, five from its centre
constexpr int kSoak = 300;

/// The ground the whole rig stands on is flat, asserted so the test says what it tests.
[[nodiscard]] bool open_ground() {
    for (int y = kRowY - 8; y <= kRowY + 5; ++y) {
        for (int x = 56; x <= 82; ++x) {
            if (world::sample(kSeed, x, y).terrain != world::Terrain::Flat) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] Spawn spawn(UnitKind kind, Side side, int x, int y, std::uint16_t hp = 0) {
    Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = y;
    s.hp = hp;
    return s;
}

/// A rig: the scenario's spawns, and an unarmed enemy factory far away so nothing is shot.
[[nodiscard]] Sim rig(std::vector<Spawn> spawns) {
    Scenario scenario{};
    scenario.seed = kSeed;
    scenario.spawns = std::move(spawns);
    scenario.spawns.push_back(spawn(UnitKind::Factory, Side::Enemy, 200, 200));
    return Sim::create(scenario);
}

void run(Sim& sim, int ticks) {
    for (int t = 0; t < ticks; ++t) {
        sim.step();
    }
}

/// What `unit` has been mended by, in the sim's own currency: whole hit points are
/// `kServiceRepairHitPoint` of the remainder each.
[[nodiscard]] std::int64_t accrued(const Unit& unit) {
    return static_cast<std::int64_t>(unit.hp) * kServiceRepairHitPoint
           + static_cast<std::int64_t>(unit.repair_frac);
}

[[nodiscard]] bool inside(const Unit& bay, const Unit& unit) {
    return footprint_around(bay).covers(unit.x.floor_int(), unit.y.floor_int());
}

/// Orders unit `id` to the middle of the platform and steps until it stands inside it (or gives
/// up). True when it got there.
[[nodiscard]] bool drive_onto(Sim& sim, UnitId id, const Unit& bay, int give_up = 600) {
    sim.enqueue(Order{id, orders::MoveTo{bay.x, bay.y}});
    for (int t = 0; t < give_up; ++t) {
        sim.step();
        if (inside(*sim.find(bay.id), *sim.find(id))) {
            return true;
        }
    }
    return false;
}

class Platform : public ::testing::TestWithParam<UnitKind> {};

/// unit 0 = the platform, unit 1 = the damaged tank one tile off its east edge.
[[nodiscard]] Sim parked_beside(UnitKind bay, std::uint16_t tank_hp = 1,
                                Side tank_side = Side::Player) {
    return rig({spawn(bay, Side::Player, kBayX, kBayY),
                spawn(UnitKind::Tank, tank_side, kApproachX, kRowY, tank_hp)});
}

TEST(Platform, TheGroundIsOpen) { ASSERT_TRUE(open_ground()); }

TEST_P(Platform, ADamagedUnitStandingOnItIsMended) {
    Sim sim = parked_beside(GetParam());
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    ASSERT_TRUE(drive_onto(sim, tank, *sim.find(bay))) << "it must be able to drive onto it";
    const int before = sim.find(tank)->hp;
    run(sim, kSoak);
    EXPECT_GT(sim.find(tank)->hp, before + 30) << "his bar must rise while it is parked there";
    EXPECT_TRUE(inside(*sim.find(bay), *sim.find(tank)));
}

TEST_P(Platform, ADamagedUnitNearButNotOnItIsNot) {
    // His exact complaint, the half that must stay true: "if I'm in its vicinity, nothing gets
    // repaired". The tank stands one tile off the edge -- within the truck's six tiles of the
    // platform's centre, asserted -- and its bar stays where it is.
    Sim sim = parked_beside(GetParam());
    const Unit& bay = sim.units().at(0);
    const Unit& tank = sim.units().at(1);
    ASSERT_FALSE(inside(bay, tank));
    const std::int64_t radius = static_cast<std::int64_t>(kServiceRepairRadiusTiles) * Fixed::kOne;
    const std::int64_t dx = tank.x.raw - bay.x.raw;
    const std::int64_t dy = tank.y.raw - bay.y.raw;
    ASSERT_LE(dx * dx + dy * dy, radius * radius) << "near enough that 'near' would repair";
    run(sim, kSoak);
    EXPECT_EQ(sim.units().at(1).hp, 1);
    EXPECT_EQ(sim.units().at(1).repair_frac, 0U);
}

TEST_P(Platform, AWholeUnitGainsNothing) {
    Sim sim = parked_beside(GetParam(), 0);
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    ASSERT_TRUE(drive_onto(sim, tank, *sim.find(bay)));
    run(sim, kSoak);
    EXPECT_EQ(sim.find(tank)->hp, kTankHp);
    EXPECT_EQ(sim.find(tank)->repair_frac, 0U);
}

TEST_P(Platform, AnEnemyOnYourPlatformIsNotMended) {
    // The bay is the player's; the tank on it is the enemy's. (An enemy rig: no shooting, the
    // player has no gun on the field, and the enemy tank is the only thing that moves.)
    Sim sim = parked_beside(GetParam(), 1, Side::Enemy);
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    ASSERT_TRUE(drive_onto(sim, tank, *sim.find(bay))) << "a platform is not a wall to anyone";
    run(sim, kSoak);
    EXPECT_EQ(sim.find(tank)->hp, 1);
}

TEST_P(Platform, ADestroyedPlatformMendsNothing) {
    // The tank is driven on, then an enemy gun twelve tiles west of the platform's edge kills the
    // platform (one hit point) -- the tank stands nineteen tiles from that gun, out of its reach,
    // so nothing else touches it.
    std::vector<Spawn> spawns{spawn(GetParam(), Side::Player, kBayX, kBayY, 1),
                              spawn(UnitKind::Tank, Side::Player, kApproachX, kRowY, 1),
                              spawn(UnitKind::Tank, Side::Enemy, kBayX - 12, kRowY)};
    Sim sim = rig(std::move(spawns));
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    const UnitId gun = sim.units().at(2).id;
    ASSERT_TRUE(drive_onto(sim, tank, *sim.find(bay)));
    ASSERT_TRUE(sim.find(bay)->alive);
    sim.enqueue(Order{gun, orders::Attack{bay}});
    run(sim, 120);
    ASSERT_FALSE(sim.find(bay)->alive) << "the platform was to be destroyed";
    ASSERT_TRUE(sim.find(tank)->alive);
    const std::int64_t then = accrued(*sim.find(tank));
    run(sim, kSoak);
    EXPECT_EQ(accrued(*sim.find(tank)), then) << "a wreck repairs nothing";
}

TEST_P(Platform, TheRateIsTheServiceTrucksOwn) {
    // One tank mended by a truck standing on it (distance zero: the truck's full rate, in front of
    // it by construction), one mended by the platform: the same number of ticks, the same gain,
    // exactly, in the sim's own currency -- and so the same hull.
    Sim truck = rig({spawn(UnitKind::Tank, Side::Player, kApproachX, kRowY, 1),
                     spawn(UnitKind::ServiceTruck, Side::Player, kApproachX, kRowY)});
    ASSERT_EQ(truck.units().at(0).x.raw, truck.units().at(1).x.raw) << "the truck is ON the tank";
    ASSERT_EQ(truck.units().at(0).y.raw, truck.units().at(1).y.raw);
    const std::int64_t truck_before = accrued(truck.units().at(0));
    run(truck, kSoak);
    const std::int64_t by_truck = accrued(truck.units().at(0)) - truck_before;
    EXPECT_EQ(by_truck, static_cast<std::int64_t>(kTankHp) * kServiceRepairFull * kSoak)
        << "the truck's own rate at its own spot";

    Sim sim = parked_beside(GetParam());
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    ASSERT_TRUE(drive_onto(sim, tank, *sim.find(bay)));
    const std::int64_t bay_before = accrued(*sim.find(tank));
    run(sim, kSoak);
    const std::int64_t by_bay = accrued(*sim.find(tank)) - bay_before;
    EXPECT_EQ(by_bay, by_truck) << "the platform mends at the truck's rate, not a second one";
}

TEST_P(Platform, AUnitMayDriveOntoItAndItsPositionLiesInside) {
    Sim sim = parked_beside(GetParam());
    const UnitId bay = sim.units().at(0).id;
    const UnitId tank = sim.units().at(1).id;
    ASSERT_FALSE(inside(*sim.find(bay), *sim.find(tank)));
    EXPECT_TRUE(drive_onto(sim, tank, *sim.find(bay)));
    EXPECT_TRUE(inside(*sim.find(bay), *sim.find(tank)));
}

TEST_P(Platform, NoBuildingMayBePlacedInsideIt) {
    // Occupied ground even though it is drivable: a placement refuses every tile of it, and one
    // that overlaps by a single tile too.
    Sim sim = parked_beside(GetParam());
    const std::span<const Unit> units{sim.units()};
    EXPECT_FALSE(place_ok(kSeed, units, UnitKind::Power, false, kBayX + 2, kBayY + 2));
    EXPECT_FALSE(place_ok(kSeed, units, UnitKind::Power, false, kBayX + 7, kBayY + 7));
    EXPECT_FALSE(place_ok(kSeed, units, UnitKind::Power, false, kBayX - 3, kBayY - 3))
        << "a 4 x 4 whose south-east tile is the platform's north-west one";
    EXPECT_TRUE(place_ok(kSeed, units, UnitKind::Power, false, kBayX + 8, kBayY))
        << "just beside it, the ground is free (the control)";
}

INSTANTIATE_TEST_SUITE_P(BothSides, Platform,
                         ::testing::Values(UnitKind::ServiceBay, UnitKind::Dower),
                         [](const ::testing::TestParamInfo<UnitKind>& info) {
                             return info.param == UnitKind::ServiceBay ? "ServiceBay" : "Dower";
                         });

TEST(Platform, TheDrivableFootprintIsExactlyTheBayAndTheDower) {
    // The exception is two kinds wide (one per side) and every other footprint blocks. A platform
    // has a footprint (it is occupied ground) and is the building's hit shape all the same.
    for (int k = 0; k < kUnitKindCount; ++k) {
        const auto kind = static_cast<UnitKind>(k);
        const bool platform = kind == UnitKind::ServiceBay || kind == UnitKind::Dower;
        EXPECT_EQ(has_drivable_footprint(kind), platform) << spec_of(kind).name;
        if (platform) {
            EXPECT_TRUE(has_footprint(kind)) << spec_of(kind).name;
        }
    }
}

TEST(Platform, TheRateConstantsAreTheTrucksOwnNotACopy) {
    // The platform asks `service_repair_weight` itself: a truck standing exactly on a patient gives
    // `kServiceRepairFull`, and that is what a bay gives, whatever way the bay "faces".
    Sim sim = rig({spawn(UnitKind::ServiceTruck, Side::Player, kApproachX, kRowY),
                   spawn(UnitKind::Tank, Side::Player, kApproachX, kRowY, 1)});
    EXPECT_EQ(service_repair_weight(sim.units().at(0), sim.units().at(1)), kServiceRepairFull);
}

TEST(Platform, AnOrdinaryBuildingStillBlocksTheSameMove) {
    // The control: the very same drive, onto a POWER plant (4 x 4 there, the same 8 x 8 would be
    // the bay's -- the footprint's size is not the point, its being drivable is). The tank is
    // refused its footprint, and never stands inside.
    for (const UnitKind kind : {UnitKind::Power, UnitKind::Armory, UnitKind::Kiln}) {
        Sim sim = rig({spawn(kind, Side::Player, kBayX, kBayY),
                       spawn(UnitKind::Tank, Side::Player, kApproachX, kRowY, 1)});
        const UnitId tank = sim.units().at(1).id;
        const Unit building = sim.units().at(0);
        EXPECT_FALSE(drive_onto(sim, tank, building, 400)) << spec_of(kind).name;
        EXPECT_FALSE(inside(*sim.find(building.id), *sim.find(tank))) << spec_of(kind).name;
        EXPECT_EQ(sim.find(tank)->hp, 1) << spec_of(kind).name << " mends nothing either";
    }
}

} // namespace
} // namespace protekt::sim
