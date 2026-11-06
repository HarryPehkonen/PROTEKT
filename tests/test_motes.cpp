// FLOATING MOTES (AH). Harri: "I would eventually like particles floating in the air for effect."
// A dozen specks near the viewer, rebuilt every frame from the frame's seed and the sim's tick, so
// they are VIEW-ONLY: nothing about them is stored, and no battle can depend on them.
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::game::build_scene;
using protekt::game::kMetresPerTile;
using protekt::game::kMoteCount;
using protekt::game::kSphereVertices;
using protekt::game::Scene;
using protekt::game::SceneOptions;
using protekt::game::SceneVertex;
using protekt::sim::Sim;

constexpr std::uint64_t kSeed = 77;

Sim rig() {
    protekt::sim::Scenario s;
    s.seed = kSeed;
    s.spawns = {{protekt::sim::UnitKind::Tank, protekt::sim::Side::Player, 60, 60},
                {protekt::sim::UnitKind::Tank, protekt::sim::Side::Enemy, 90, 60}};
    return Sim::create(s);
}

SceneOptions view(bool motes, float x = 120.0F, float z = 120.0F) {
    SceneOptions o;
    o.seed = kSeed;
    o.radius_tiles = 20;
    o.camera.x = x;
    o.camera.z = z;
    o.camera.y = 6.0F;
    o.show_ship_gauge = false;
    o.show_ledger = false;
    o.show_motes = motes;
    return o;
}

/// The motes of a frame: the vertices that frame has over the same frame without them. They are
/// inserted in the world layer, so the first vertex that differs is the first mote's.
std::vector<SceneVertex> motes_of(const Sim& sim, SceneOptions options) {
    options.show_motes = false;
    const Scene off = build_scene(sim, options);
    options.show_motes = true;
    const Scene on = build_scene(sim, options);
    std::vector<SceneVertex> found;
    if (on.vertices.size() < off.vertices.size()) {
        return found;
    }
    std::size_t first = 0;
    while (first < off.vertices.size()
           && on.vertices.at(first).position == off.vertices.at(first).position
           && on.vertices.at(first).colour == off.vertices.at(first).colour) {
        ++first;
    }
    const std::size_t count = on.vertices.size() - off.vertices.size();
    for (std::size_t i = first; i < first + count; ++i) {
        found.push_back(on.vertices.at(i));
    }
    return found;
}

std::array<float, 3> centre_of(const std::vector<SceneVertex>& v, std::size_t mote) {
    std::array<float, 3> sum{};
    for (std::size_t i = 0; i < kSphereVertices; ++i) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            sum.at(axis) += v.at((mote * kSphereVertices) + i).position.at(axis);
        }
    }
    for (float& c : sum) {
        c /= static_cast<float>(kSphereVertices);
    }
    return sum;
}

float apart(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::sqrt(((a[0] - b[0]) * (a[0] - b[0])) + ((a[1] - b[1]) * (a[1] - b[1]))
                     + ((a[2] - b[2]) * (a[2] - b[2])));
}

std::array<float, 3> facing_of(const std::vector<SceneVertex>& v, std::size_t mote) {
    const std::array<float, 3> centre = centre_of(v, mote);
    std::array<float, 3> d{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        d.at(axis) = v.at(mote * kSphereVertices).position.at(axis) - centre.at(axis);
    }
    const float length = apart(d, {0.0F, 0.0F, 0.0F});
    return {d[0] / length, d[1] / length, d[2] / length};
}

TEST(Motes, AreOffUnlessAskedForAndAFrameWithoutThemIsUnchanged) {
    const Sim sim = rig();
    EXPECT_FALSE(SceneOptions{}.show_motes);
    EXPECT_EQ(build_scene(sim, view(true)).vertices.size()
                  - build_scene(sim, view(false)).vertices.size(),
              kMoteCount * kSphereVertices);
}

TEST(Motes, AreADozenOrSoOfIcospheresAndNeverMore) {
    // The frame budget is the constraint: every mote is geometry rebuilt each frame.
    EXPECT_GE(kMoteCount, std::size_t{8});
    EXPECT_LE(kMoteCount, std::size_t{16});
    const Sim sim = rig();
    EXPECT_EQ(motes_of(sim, view(true)).size(), kMoteCount * kSphereVertices);
    EXPECT_EQ(kSphereVertices, std::size_t{60});
}

TEST(Motes, StayWithinTheBoxAroundTheViewerAndAboveTheGround) {
    const Sim sim = rig();
    for (const float x : {120.0F, 333.3F, 17.0F}) {
        for (const float z : {120.0F, 250.5F, 41.0F}) {
            const SceneOptions options = view(true, x, z);
            const std::vector<SceneVertex> v = motes_of(sim, options);
            ASSERT_EQ(v.size(), kMoteCount * kSphereVertices);
            for (std::size_t mote = 0; mote < kMoteCount; ++mote) {
                const std::array<float, 3> c = centre_of(v, mote);
                const float half = protekt::game::kMoteBoxMetres * 0.5F;
                EXPECT_LE(std::abs(c[0] - x), half + 0.1F) << "a mote left the viewer's box";
                EXPECT_LE(std::abs(c[2] - z), half + 0.1F) << "a mote left the viewer's box";
                const float ground = protekt::game::ground_metres(kSeed, c[0] / kMetresPerTile,
                                                                  c[2] / kMetresPerTile);
                EXPECT_GT(c[1], ground) << "a mote is in the hill";
                EXPECT_LT(c[1], ground + protekt::game::kMoteCeilingMetres + 1.0F);
            }
        }
    }
}

TEST(Motes, HangInTheWorldNotOnTheLens) {
    // Move the camera a metre: the motes stay where they are (bar the odd one wrapping round).
    const Sim sim = rig();
    const std::vector<SceneVertex> a = motes_of(sim, view(true, 120.0F, 120.0F));
    const std::vector<SceneVertex> b = motes_of(sim, view(true, 121.0F, 120.0F));
    ASSERT_EQ(a.size(), b.size());
    int still = 0;
    for (std::size_t mote = 0; mote < kMoteCount; ++mote) {
        if (apart(centre_of(a, mote), centre_of(b, mote)) < 1e-3F) {
            ++still;
        }
    }
    EXPECT_GE(still, static_cast<int>(kMoteCount) - 2);
}

TEST(Motes, DriftAsTheTicksPassAndTurnAsTheyGo) {
    Sim sim = rig();
    const std::vector<SceneVertex> before = motes_of(sim, view(true));
    for (int i = 0; i < 20; ++i) {
        sim.step();
    }
    const std::vector<SceneVertex> after = motes_of(sim, view(true));
    ASSERT_EQ(before.size(), after.size());
    int moved = 0;
    int turned = 0;
    for (std::size_t mote = 0; mote < kMoteCount; ++mote) {
        moved += apart(centre_of(before, mote), centre_of(after, mote)) > 0.05F ? 1 : 0;
        turned += apart(facing_of(before, mote), facing_of(after, mote)) > 1e-3F ? 1 : 0;
    }
    EXPECT_GE(moved, static_cast<int>(kMoteCount) - 2) << "the motes do not drift";
    EXPECT_GE(turned, static_cast<int>(kMoteCount) - 2) << "the motes do not roll";
}

TEST(Motes, NoTwoShareAnOrientation) {
    const std::vector<SceneVertex> v = motes_of(rig(), view(true));
    ASSERT_EQ(v.size(), kMoteCount * kSphereVertices);
    for (std::size_t a = 0; a < kMoteCount; ++a) {
        for (std::size_t b = a + 1; b < kMoteCount; ++b) {
            EXPECT_GT(apart(facing_of(v, a), facing_of(v, b)), 1e-3F)
                << "motes " << a << " and " << b << " are turned the same way";
        }
    }
}

TEST(Motes, ComeOffTheFramesSeed) {
    const Sim sim = rig();
    SceneOptions other = view(true);
    other.seed = kSeed + 1;
    const std::vector<SceneVertex> a = motes_of(sim, view(true));
    const std::vector<SceneVertex> b = motes_of(sim, other);
    ASSERT_EQ(a.size(), b.size());
    int differ = 0;
    for (std::size_t mote = 0; mote < kMoteCount; ++mote) {
        differ += apart(centre_of(a, mote), centre_of(b, mote)) > 0.1F ? 1 : 0;
    }
    EXPECT_GE(differ, static_cast<int>(kMoteCount) - 1);
}

TEST(Motes, AreFaintAndFadeOutAtTheEdgeOfTheirBox) {
    const std::vector<SceneVertex> v = motes_of(rig(), view(true));
    float brightest = 0.0F;
    for (const SceneVertex& vertex : v) {
        EXPECT_GE(vertex.alpha, 0.0F);
        EXPECT_LE(vertex.alpha, protekt::game::kMoteAlpha + 1e-4F);
        brightest = std::max(brightest, vertex.alpha);
    }
    EXPECT_GT(brightest, 0.0F) << "every mote is invisible";
    EXPECT_LT(protekt::game::kMoteAlpha, 1.0F) << "a mote is a see-through speck";
}

TEST(Motes, AreViewOnlyAndNoRecordedBattleMovesForThem) {
    // Two copies of one battle: one drawn every tick with motes, one never drawn at all. The
    // scene reads the sim by const reference, and the proof is the hash, tick for tick.
    Sim watched = rig();
    Sim blind = rig();
    for (int tick = 0; tick < 120; ++tick) {
        const Scene scene = build_scene(watched, view(true));
        EXPECT_FALSE(scene.vertices.empty());
        watched.step();
        blind.step();
        ASSERT_EQ(watched.state_hash(), blind.state_hash()) << "tick " << tick;
    }
}

} // namespace
