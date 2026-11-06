// THE REPAIR BEAM (D2, the look for D1's repair). Harri, 2026-11-05: "a bright, trembling beam of
// different colours coming from the service truck to the item being repaired." Drawn only while a
// repair is actually advancing; the scene reads the sim's units and the tick and nothing else.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"
#include "protekt/sim/spec.hpp"

namespace {

using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::SceneVertex;
using protekt::sim::Side;
using protekt::sim::Sim;
using protekt::sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kRowY = 60;
constexpr int kPatientX = 60;
constexpr int kTruckX = 64;

protekt::sim::Spawn spawn(UnitKind kind, Side side, int x, std::uint16_t hp = 0) {
    protekt::sim::Spawn s{};
    s.kind = kind;
    s.side = side;
    s.tile_x = x;
    s.tile_y = kRowY;
    s.hp = hp;
    return s;
}

/// A patient at `kPatientX` with `patient_hp` (0 = whole) and a friendly truck four tiles away.
Sim rig(std::uint16_t patient_hp, Side truck_side = Side::Player) {
    protekt::sim::Scenario s;
    s.seed = kSeed;
    s.spawns = {spawn(UnitKind::Tank, Side::Player, kPatientX, patient_hp),
                spawn(UnitKind::ServiceTruck, truck_side, kTruckX),
                spawn(UnitKind::Factory, Side::Enemy, 200, 0)};
    s.spawns.back().tile_y = 200;
    return Sim::create(s);
}

SceneOptions view() {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 14;
    o.camera.x = static_cast<float>(kPatientX + 2) * protekt::game::kMetresPerTile;
    o.camera.z = static_cast<float>(kRowY - 8) * protekt::game::kMetresPerTile;
    o.camera.y = 9.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    return o;
}

bool is_beam_ink(const SceneVertex& v) {
    return std::ranges::any_of(protekt::game::kRepairBeamPalette, [&](const auto& ink) {
        return std::abs(v.colour.at(0) - ink.at(0)) < 0.002F
               && std::abs(v.colour.at(1) - ink.at(1)) < 0.002F
               && std::abs(v.colour.at(2) - ink.at(2)) < 0.002F;
    });
}

std::vector<SceneVertex> beam_of(const Scene& scene) {
    std::vector<SceneVertex> found;
    for (const SceneVertex& v : scene.vertices) {
        if (is_beam_ink(v)) {
            found.push_back(v);
        }
    }
    return found;
}

TEST(RepairBeam, ARepairingTruckDrawsABeam) {
    const Sim sim = rig(1);
    ASSERT_EQ(protekt::game::repair_links(sim).size(), 1U);
    EXPECT_FALSE(beam_of(protekt::game::build_scene(sim, view())).empty());
}

TEST(RepairBeam, ATruckBesideAFullHealthFriendDrawsNone) {
    const Sim sim = rig(0);
    EXPECT_TRUE(protekt::game::repair_links(sim).empty());
    EXPECT_TRUE(beam_of(protekt::game::build_scene(sim, view())).empty());
}

TEST(RepairBeam, AnEnemyTruckDoesNotMendMyTankSoItDrawsNone) {
    const Sim sim = rig(1, Side::Enemy);
    EXPECT_TRUE(protekt::game::repair_links(sim).empty());
    EXPECT_TRUE(beam_of(protekt::game::build_scene(sim, view())).empty());
}

TEST(RepairBeam, ATruckOutOfReachDrawsNone) {
    protekt::sim::Scenario s;
    s.seed = kSeed;
    s.spawns = {spawn(UnitKind::Tank, Side::Player, kPatientX, 1),
                spawn(UnitKind::ServiceTruck, Side::Player,
                      kPatientX + protekt::sim::kServiceRepairRadiusTiles + 1)};
    EXPECT_TRUE(protekt::game::repair_links(Sim::create(s)).empty());
}

TEST(RepairBeam, TheLinkNamesTheTruckAndThePatient) {
    const Sim sim = rig(1);
    const auto links = protekt::game::repair_links(sim);
    ASSERT_EQ(links.size(), 1U);
    EXPECT_EQ(sim.find(links.at(0).patient)->kind, UnitKind::Tank);
    EXPECT_EQ(sim.find(links.at(0).truck)->kind, UnitKind::ServiceTruck);
}

TEST(RepairBeam, TheBeamRunsFromTheTruckToThePatient) {
    const Sim sim = rig(1);
    const std::vector<SceneVertex> beam = beam_of(protekt::game::build_scene(sim, view()));
    ASSERT_FALSE(beam.empty());
    float lo = beam.front().position.at(0);
    float hi = lo;
    for (const SceneVertex& v : beam) {
        lo = std::min(lo, v.position.at(0));
        hi = std::max(hi, v.position.at(0));
    }
    const auto centre_x = [&](std::size_t index) {
        return static_cast<float>(sim.units().at(index).x.raw)
               / static_cast<float>(protekt::sim::Fixed::kOne) * protekt::game::kMetresPerTile;
    };
    const float pad = protekt::game::kRepairBeamBeadHalfMetres + 0.01F;
    const float patient_x = centre_x(0);
    const float truck_x = centre_x(1);
    // It reaches both: the end beads sit on the endpoints (the tremble fades to nothing there)...
    EXPECT_NEAR(lo, patient_x - protekt::game::kRepairBeamBeadHalfMetres, pad);
    EXPECT_NEAR(hi, truck_x + protekt::game::kRepairBeamBeadHalfMetres, pad);
    // ...and does not run past either.
    EXPECT_GE(lo, patient_x - pad);
    EXPECT_LE(hi, truck_x + pad);
}

TEST(RepairBeam, ItIsMultiColoured) {
    const Sim sim = rig(1);
    std::vector<std::array<float, 3>> inks;
    for (const SceneVertex& v : beam_of(protekt::game::build_scene(sim, view()))) {
        if (std::ranges::find(inks, v.colour) == inks.end()) {
            inks.push_back(v.colour);
        }
    }
    EXPECT_EQ(inks.size(), protekt::game::kRepairBeamPalette.size());
}

TEST(RepairBeam, ItTremblesFromOneTickToTheNext) {
    Sim sim = rig(1);
    const std::vector<SceneVertex> before = beam_of(protekt::game::build_scene(sim, view()));
    sim.step();
    const std::vector<SceneVertex> after = beam_of(protekt::game::build_scene(sim, view()));
    ASSERT_EQ(before.size(), after.size());
    int moved = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (std::abs(before.at(i).position.at(1) - after.at(i).position.at(1)) > 0.01F) {
            ++moved;
        }
    }
    EXPECT_GT(moved, static_cast<int>(before.size()) / 4);
}

TEST(RepairBeam, TheSameTickDrawsTheSameBytes) {
    const Sim sim = rig(1);
    const std::vector<SceneVertex> a = beam_of(protekt::game::build_scene(sim, view()));
    const std::vector<SceneVertex> b = beam_of(protekt::game::build_scene(sim, view()));
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a.at(i).position, b.at(i).position);
    }
}

TEST(RepairBeam, TheBeamGoesWhenThePatientIsWhole) {
    // Mend a nearly dead tank until the sim says it is whole: the beam stops with the repair.
    Sim sim = rig(1);
    const std::uint16_t whole = protekt::sim::profile(UnitKind::Tank).hp;
    int ticks = 0;
    while (sim.units().at(0).hp < whole && ticks < 4000) {
        EXPECT_FALSE(protekt::game::repair_links(sim).empty()) << "hp " << sim.units().at(0).hp;
        sim.step();
        ++ticks;
    }
    ASSERT_EQ(sim.units().at(0).hp, whole);
    EXPECT_TRUE(protekt::game::repair_links(sim).empty());
    EXPECT_TRUE(beam_of(protekt::game::build_scene(sim, view())).empty());
}

TEST(RepairBeam, ALinkMeansTheSimIsAdvancingThatRepair) {
    // The scene's question is the sim's: whenever a link stands, one more step moves the patient's
    // hit points or its carried remainder; whenever none stands, neither moves.
    Sim sim = rig(1);
    for (int t = 0; t < 400; ++t) {
        const bool linked = !protekt::game::repair_links(sim).empty();
        const protekt::sim::Unit before = sim.units().at(0);
        sim.step();
        const protekt::sim::Unit& after = sim.units().at(0);
        const bool advanced = after.hp != before.hp || after.repair_frac != before.repair_frac;
        EXPECT_EQ(linked, advanced) << "tick " << t;
    }
}

} // namespace
