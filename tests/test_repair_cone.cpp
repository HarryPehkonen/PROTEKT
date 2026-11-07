#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"
#include "protekt/sim/units.hpp"
#include "protekt/world/world.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <vector>

/// THE SHOTGUN BEAM (AJ). A truck mends what lies ahead of it: the share a patient gets is the
/// distance weight (D1, unchanged) times cos^2 of the angle between the truck's FACING and the line
/// to the patient -- 1.00 dead ahead, 0.75 at 30 degrees, 0.50 at 45, 0.25 at 60, 0.00 at 90 -- and
/// NOTHING behind the truck. Harri's "anything within their repair distance" survives as the
/// reconciliation: everything in reach and in the front half is mended, the centre the most.
///
/// Every angle below is made by turning the TRUCK (`Spawn::face_x/face_y`) and leaving the patient
/// three tiles due +Y of it, so the distance weight is the same in every test and only the cone
/// differs. The ground is asserted open; the enemy base is unarmed and far away.
namespace protekt::sim {
namespace {

constexpr std::uint64_t kSeed = 42;
constexpr int kRowY = 23;
constexpr int kTruckX = 113;
constexpr int kSoak = 300; ///< ticks: half a full repair, so a rate is read well clear of its cap
constexpr int kThree = 3;

/// The weight a patient three tiles away gets from a truck at full cone: the distance rule alone.
constexpr std::int64_t kAheadWeight
    = kServiceRepairFull
      - (kServiceRepairFull - kServiceRepairEdge) * kThree * kThree
            / (static_cast<std::int64_t>(kServiceRepairRadiusTiles) * kServiceRepairRadiusTiles);

[[nodiscard]] bool open_ground() {
    for (int y = kRowY - 8; y <= kRowY + 8; ++y) {
        for (int x = kTruckX - 8; x <= kTruckX + 8; ++x) {
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

[[nodiscard]] Spawn truck_facing(int face_x, int face_y) {
    Spawn s = spawn(UnitKind::ServiceTruck, Side::Player, kTruckX, kRowY);
    s.face_x = face_x;
    s.face_y = face_y;
    return s;
}

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

/// A wounded tank `dx`, `dy` tiles from a truck that faces (`face_x`, `face_y`): the truck is unit
/// 0, the patient unit 1.
[[nodiscard]] Sim ward(int face_x, int face_y, int dx, int dy) {
    return rig({truck_facing(face_x, face_y),
                spawn(UnitKind::Tank, Side::Player, kTruckX + dx, kRowY + dy, 1)});
}

/// The weight the sim gives the pair, and the hit points the patient gains in a soak.
[[nodiscard]] std::int64_t weight(const Sim& sim) {
    return service_repair_weight(sim.units().at(0), sim.units().at(1));
}

[[nodiscard]] int gained(Sim& sim) {
    run(sim, kSoak);
    return static_cast<int>(sim.units().at(1).hp) - 1;
}

/// Whole units of weight at an angle off the nose, the patient three tiles due +Y. The truck faces
/// (`face_x`, `face_y`); both are in whole numbers because a facing is a direction, not a length.
[[nodiscard]] std::int64_t weight_facing(int face_x, int face_y) {
    return weight(ward(face_x, face_y, 0, kThree));
}

TEST(RepairCone, TheGroundIsOpen) { ASSERT_TRUE(open_ground()); }

TEST(RepairCone, DeadAheadIsTheFullRate) {
    // A truck's default facing is +Y, as a stopped hull is drawn. The patient is due +Y: the cone
    // takes nothing off the distance rule, so the sim's number is D1's own.
    EXPECT_EQ(weight_facing(0, 0), kAheadWeight);
    EXPECT_EQ(weight_facing(0, 1), kAheadWeight);
    Sim sim = ward(0, 1, 0, kThree);
    // 100 hp over 600 ticks at 0.875 of the full rate, 300 ticks: 43.75 hit points.
    EXPECT_NEAR(gained(sim), 43, 1);
}

TEST(RepairCone, NinetyDegreesOffTheNoseGetsNothing) {
    // Harri: "90 degrees to the side, I would say no effect."
    EXPECT_EQ(weight_facing(1, 0), 0);
    EXPECT_EQ(weight_facing(-1, 0), 0);
    Sim sim = ward(1, 0, 0, kThree);
    EXPECT_EQ(gained(sim), 0);
    EXPECT_EQ(sim.units().at(1).repair_frac, 0U);
}

TEST(RepairCone, BehindTheTruckGetsNothing) {
    // Harri: "I don't want anything BEHIND a service vehicle repaired." Dead behind, and well
    // behind at an angle, and just past the beam's side.
    EXPECT_EQ(weight_facing(0, -1), 0);
    EXPECT_EQ(weight_facing(1, -1), 0);
    EXPECT_EQ(weight_facing(1, -3), 0);
    EXPECT_EQ(weight_facing(-1, -4), 0);
    Sim sim = ward(0, -1, 0, kThree);
    EXPECT_EQ(gained(sim), 0);
    EXPECT_EQ(sim.units().at(1).repair_frac, 0U);
}

TEST(RepairCone, FortyFiveDegreesGetsAboutHalf) {
    const std::int64_t half = weight_facing(1, 1);
    EXPECT_NEAR(static_cast<double>(half), static_cast<double>(kAheadWeight) / 2.0,
                static_cast<double>(kAheadWeight) / 200.0);
    Sim sim = ward(1, 1, 0, kThree);
    EXPECT_NEAR(gained(sim), 22, 1);
}

TEST(RepairCone, TheFallOffIsCosineSquared) {
    // The profile Harri named, a shotgun microphone: .75 at 30 degrees, .5 at 45, .25 at 60. The
    // truck turns; the patient stays put. (tan 30 = 0.577, tan 60 = 1.732.)
    const auto share = [](int face_x, int face_y) {
        return static_cast<double>(weight_facing(face_x, face_y))
               / static_cast<double>(kAheadWeight);
    };
    EXPECT_NEAR(share(0, 1), 1.00, 0.005);
    EXPECT_NEAR(share(577, 1000), 0.75, 0.005);
    EXPECT_NEAR(share(1, 1), 0.50, 0.005);
    EXPECT_NEAR(share(1732, 1000), 0.25, 0.005);
    EXPECT_EQ(weight_facing(1000, 0), 0);
}

TEST(RepairCone, TheCloserToTheBeamTheMoreItGets) {
    // The truck turned 30 degrees right of +Y; two patients at the same distance, one 30 degrees
    // off the beam (due +Y) and the other 60 degrees off (due +X). Both are three tiles out, so
    // only the cone tells them apart: three quarters against a quarter.
    Sim sim = rig({truck_facing(577, 1000),
                   spawn(UnitKind::Tank, Side::Player, kTruckX, kRowY + kThree, 1),
                   spawn(UnitKind::Tank, Side::Player, kTruckX + kThree, kRowY, 1)});
    run(sim, kSoak);
    const int near_beam = static_cast<int>(sim.units().at(1).hp) - 1;
    const int far_off = static_cast<int>(sim.units().at(2).hp) - 1;
    EXPECT_GT(near_beam, far_off);
    EXPECT_NEAR(near_beam, 32, 1);
    EXPECT_NEAR(far_off, 11, 1);
}

TEST(RepairCone, TheReachStillAdmitsEverythingInTheFrontHalf) {
    // The reconciliation with "anything within their repair distance": over every tile around a
    // truck facing +Y, a patient is mended exactly when it is in reach and ahead of the truck's
    // beam-line (the front half); nothing in reach is dropped but what is behind or abreast.
    const std::int64_t reach2
        = static_cast<std::int64_t>(kServiceRepairRadiusTiles) * kServiceRepairRadiusTiles;
    int mended = 0;
    int dropped_in_reach = 0;
    for (int dy = -kServiceRepairRadiusTiles; dy <= kServiceRepairRadiusTiles; ++dy) {
        for (int dx = -kServiceRepairRadiusTiles; dx <= kServiceRepairRadiusTiles; ++dx) {
            if (dx == 0 && dy == 0) {
                continue;
            }
            const Sim sim = ward(0, 1, dx, dy);
            const bool in_reach
                = static_cast<std::int64_t>(dx) * dx + static_cast<std::int64_t>(dy) * dy <= reach2;
            const bool ahead = dy > 0;
            EXPECT_EQ(weight(sim) > 0, in_reach && ahead) << "offset " << dx << ", " << dy;
            mended += weight(sim) > 0 ? 1 : 0;
            dropped_in_reach += (in_reach && ahead && weight(sim) == 0) ? 1 : 0;
        }
    }
    EXPECT_GT(mended, 40) << "a whole half-disc of tiles, not a thin lane";
    EXPECT_EQ(dropped_in_reach, 0);
}

TEST(RepairCone, AnOrderedPatientTheTruckDroveTowardIsAhead) {
    // The beam's axis is the truck's own facing, which is where it last moved: a truck sent to a
    // patient arrives pointing at it, so the patient gets the full centre of the cone.
    Sim sim = rig({spawn(UnitKind::ServiceTruck, Side::Player, kTruckX, kRowY),
                   spawn(UnitKind::Tank, Side::Player, kTruckX + 7, kRowY, 1)});
    const Unit& truck = sim.units().at(0);
    ASSERT_EQ(truck.face_x, 0) << "not yet moved: the +Y default";
    sim.enqueue(Order{truck.id, orders::Repair{sim.units().at(1).id}});
    run(sim, 20);
    EXPECT_GT(sim.units().at(0).face_x, 1000) << "driving east: pointing east";
    EXPECT_EQ(sim.units().at(0).face_y, 0);
    EXPECT_GT(gained(sim), 40);
}

TEST(RepairCone, AFacingOnlyEntersTheHashWhenATruckHasOne) {
    // No truck, or a truck that never moved: the hash is the battle's as it was before the cone.
    Sim plain = rig({spawn(UnitKind::Tank, Side::Player, kTruckX, kRowY, 1)});
    Sim same = rig({spawn(UnitKind::Tank, Side::Player, kTruckX, kRowY, 1)});
    run(plain, 10);
    run(same, 10);
    EXPECT_EQ(plain.state_hash(), same.state_hash());
    Sim north = rig({truck_facing(0, 1)});
    Sim east = rig({truck_facing(1, 0)});
    EXPECT_NE(north.state_hash(), east.state_hash());
}

} // namespace
} // namespace protekt::sim
