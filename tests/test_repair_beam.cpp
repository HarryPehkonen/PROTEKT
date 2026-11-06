// THE REPAIR BEAM (D2, the look for D1's repair). Harri, 2026-11-05: "a bright, trembling beam of
// different colours coming from the service truck to the item being repaired." Drawn only while a
// repair is actually advancing; the scene reads the sim's units and the tick and nothing else.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
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
    const float pad = protekt::game::kRepairBeamBeadRadiusMetres + 0.01F;
    const float patient_x = centre_x(0);
    const float truck_x = centre_x(1);
    // It reaches both: the end beads sit on the endpoints (the tremble fades to nothing there)...
    EXPECT_NEAR(lo, patient_x - protekt::game::kRepairBeamBeadRadiusMetres, pad);
    EXPECT_NEAR(hi, truck_x + protekt::game::kRepairBeamBeadRadiusMetres, pad);
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

// --- AG: the beam's shapes are transparent SPHERES, and the weld throws SPARKS
// ---------------------

/// The repair's sparks, picked out of the frame by WHERE they are: the weld's sparks are emitted
/// straight after the beam's last bead, so they are the `kRepairSparks` boxes that follow the last
/// vertex in a beam ink. (Colour alone cannot pick them: a lit patch of ground can be any warm
/// colour.) Empty when there is no beam.
std::vector<SceneVertex> sparks_of(const Scene& scene) {
    std::size_t after_beam = 0;
    for (std::size_t i = 0; i < scene.vertices.size(); ++i) {
        if (is_beam_ink(scene.vertices.at(i))) {
            after_beam = i + 1;
        }
    }
    std::vector<SceneVertex> found;
    if (after_beam == 0) {
        return found;
    }
    const std::size_t count = protekt::game::kRepairSparks * protekt::game::kSparkVertices;
    for (std::size_t i = after_beam; i < after_beam + count && i < scene.vertices.size(); ++i) {
        found.push_back(scene.vertices.at(i));
    }
    return found;
}

TEST(RepairBeam, ItsShapesAreSpheresOfTwentyFacesEach) {
    const Sim sim = rig(1);
    const std::vector<SceneVertex> beam = beam_of(protekt::game::build_scene(sim, view()));
    EXPECT_EQ(protekt::game::kSphereFaces, std::size_t{20});
    EXPECT_EQ(beam.size(), protekt::game::kRepairBeamBeads * protekt::game::kSphereVertices);
    // A cube has eight corners; a sphere's vertices all stand one radius from the bead's centre.
    // Measured: every vertex of the first bead is the same distance from the bead's centre.
    const std::size_t each = protekt::game::kSphereVertices;
    std::array<float, 3> centre{};
    for (std::size_t i = 0; i < each; ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            centre.at(axis) += beam.at(i).position.at(axis) / static_cast<float>(each);
        }
    }
    for (std::size_t i = 0; i < each; ++i) {
        float sq = 0.0F;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            const float d = beam.at(i).position.at(axis) - centre.at(axis);
            sq += d * d;
        }
        EXPECT_NEAR(std::sqrt(sq), protekt::game::kRepairBeamBeadRadiusMetres, 0.01F);
    }
}

TEST(RepairBeam, ItsShapesAreFairlyTransparent) {
    const Sim sim = rig(1);
    const std::vector<SceneVertex> beam = beam_of(protekt::game::build_scene(sim, view()));
    ASSERT_FALSE(beam.empty());
    for (const SceneVertex& v : beam) {
        EXPECT_LT(v.alpha, 0.6F) << "fairly transparent";
        EXPECT_GT(v.alpha, 0.1F) << "but it must still be seen";
        EXPECT_EQ(v.alpha, protekt::game::kRepairBeamAlpha);
    }
}

TEST(RepairBeam, TheWeldThrowsSparksThatAreOpaqueAndBright) {
    const Sim sim = rig(1);
    const std::vector<SceneVertex> sparks = sparks_of(protekt::game::build_scene(sim, view()));
    ASSERT_EQ(sparks.size(), protekt::game::kRepairSparks * protekt::game::kSparkVertices);
    for (const SceneVertex& v : sparks) {
        EXPECT_EQ(v.alpha, 1.0F) << "the opposite of the beam: a soft spark is an invisible one";
        const float luma
            = (0.2126F * v.colour[0]) + (0.7152F * v.colour[1]) + (0.0722F * v.colour[2]);
        EXPECT_GE(luma, 0.75F);
    }
}

/// The width of each spark's box (every kSparkVertices vertices), along x.
std::vector<float> spark_widths(const std::vector<SceneVertex>& sparks) {
    std::vector<float> widths;
    for (std::size_t first = 0; first + protekt::game::kSparkVertices <= sparks.size();
         first += protekt::game::kSparkVertices) {
        float lo = 1e9F;
        float hi = -1e9F;
        for (std::size_t i = 0; i < protekt::game::kSparkVertices; ++i) {
            lo = std::min(lo, sparks.at(first + i).position.at(0));
            hi = std::max(hi, sparks.at(first + i).position.at(0));
        }
        widths.push_back(hi - lo);
    }
    return widths;
}

TEST(RepairBeam, ASparkIsSmallButNeverSmallerThanItsMinimumDrawnSize) {
    const Sim sim = rig(1);
    for (const float w : spark_widths(sparks_of(protekt::game::build_scene(sim, view())))) {
        EXPECT_GE(w, 2.0F * protekt::game::kRepairSparkHalfMetres - 1e-4F);
        EXPECT_LE(w, 0.25F) << "small: his instruction, not golf balls";
    }
    // From far away the floor in PIXELS takes over: ~2.5 rows wide, however far the camera.
    SceneOptions far = view();
    far.radius_tiles = 60;
    far.camera.z = static_cast<float>(kRowY) * protekt::game::kMetresPerTile - 100.0F;
    const std::vector<SceneVertex> distant = sparks_of(protekt::game::build_scene(sim, far));
    ASSERT_EQ(distant.size(), protekt::game::kRepairSparks * protekt::game::kSparkVertices);
    const float metres = 100.0F; // at least this far from every spark
    for (const float w : spark_widths(distant)) {
        EXPECT_GE(w, 2.0F * protekt::game::kSparkFloorPerMetre * metres * 0.9F);
    }
}

TEST(RepairBeam, TheSparksFlyFromTheWeldAndMoveFromOneTickToTheNext) {
    Sim sim = rig(1);
    const std::vector<SceneVertex> before = sparks_of(protekt::game::build_scene(sim, view()));
    sim.step();
    const std::vector<SceneVertex> after = sparks_of(protekt::game::build_scene(sim, view()));
    ASSERT_EQ(before.size(), after.size());
    int moved = 0;
    for (std::size_t i = 0; i < before.size(); ++i) {
        if (before.at(i).position != after.at(i).position) {
            ++moved;
        }
    }
    EXPECT_GT(moved, static_cast<int>(before.size()) / 2);
}

TEST(RepairBeam, AWeldSparkIsACubeOfEightCorners) {
    // AH: the tests pinned a spark's colour and size, not its shape. Eight distinct places, as wide
    // as tall as deep: an icosphere gives twelve and an eight-sided prism sixteen.
    const std::vector<SceneVertex> sparks = sparks_of(protekt::game::build_scene(rig(1), view()));
    ASSERT_EQ(sparks.size(), protekt::game::kRepairSparks * protekt::game::kSparkVertices);
    for (std::size_t first = 0; first < sparks.size(); first += protekt::game::kSparkVertices) {
        std::set<std::array<long, 3>> places;
        std::array<float, 3> lo{1e9F, 1e9F, 1e9F};
        std::array<float, 3> hi{-1e9F, -1e9F, -1e9F};
        for (std::size_t i = first; i < first + protekt::game::kSparkVertices; ++i) {
            const auto& at = sparks.at(i).position;
            places.insert({std::lround(at[0] * 1000.0F), std::lround(at[1] * 1000.0F),
                           std::lround(at[2] * 1000.0F)});
            for (std::size_t axis = 0; axis < 3; ++axis) {
                lo.at(axis) = std::min(lo.at(axis), at.at(axis));
                hi.at(axis) = std::max(hi.at(axis), at.at(axis));
            }
        }
        EXPECT_EQ(places.size(), std::size_t{8})
            << "spark " << first / protekt::game::kSparkVertices;
        EXPECT_NEAR(hi[0] - lo[0], hi[1] - lo[1], 2e-3F);
        EXPECT_NEAR(hi[0] - lo[0], hi[2] - lo[2], 2e-3F);
    }
}

TEST(RepairBeam, TheWeldsSparksAreExemptFromTheFogAndTheBeamIsNot) {
    // A spark is emissive: it does not fade into the horizon. The beam beside it does.
    const Sim sim = rig(1);
    const Scene scene = protekt::game::build_scene(sim, view());
    float beam_fog = 0.0F;
    for (const SceneVertex& v : beam_of(scene)) {
        beam_fog = std::max(beam_fog, v.fog);
    }
    EXPECT_GT(beam_fog, 0.0F) << "the rig is too near for the fog to touch anything";
    const std::vector<SceneVertex> sparks = sparks_of(scene);
    ASSERT_FALSE(sparks.empty());
    for (const SceneVertex& v : sparks) {
        EXPECT_EQ(v.fog, 0.0F) << "a spark faded toward the horizon";
    }
}

TEST(RepairBeam, NoRepairMeansNoSparks) {
    // The repairing frame is bigger than the idle one by the beam and the sparks, and by nothing
    // else: the sparks are part of the beam's layer, not a standing decoration.
    const std::size_t idle = protekt::game::build_scene(rig(0), view()).vertices.size();
    const std::size_t mending = protekt::game::build_scene(rig(1), view()).vertices.size();
    EXPECT_EQ(mending - idle, (protekt::game::kRepairBeamBeads * protekt::game::kSphereVertices)
                                  + (protekt::game::kRepairSparks * protekt::game::kSparkVertices));
}

} // namespace
