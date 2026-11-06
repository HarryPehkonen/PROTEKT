// THE EXTRACTED PARTICLE (AG). The blast's puffs, the wreck's smoke and the sparks are three
// callers of one spawn: a size, a colour, a lifetime and a velocity, read at an age. These tests
// pin the spawn itself; the callers' own tests (test_scene.cpp, test_repair_beam.cpp) pin that each
// one really goes through it.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "protekt/game/scene.hpp"

namespace {

using protekt::game::emit_particle;
using protekt::game::Particle;
using protekt::game::SceneVertex;

constexpr protekt::math::Vec3 kFarEye{0.0F, 0.0F, 0.0F};

Particle sample() {
    Particle p;
    p.origin = {10.0F, 5.0F, 10.0F};
    p.velocity = {1.0F, 0.5F, 0.0F};
    p.gravity = 0.1F;
    p.lifetime = 10;
    p.half_from = 1.0F;
    p.half_to = 0.5F;
    p.colour_from = {1.0F, 0.0F, 0.0F};
    p.colour_to = {0.0F, 0.0F, 1.0F};
    p.alpha_from = 0.8F;
    p.alpha_to = 0.0F;
    p.shape = protekt::game::ParticleShape::Prism;
    p.sides = protekt::game::kPuffSides;
    return p;
}

/// The same particle as a PUFF: an icosphere, standing still, so only its roll is in play.
Particle puff() {
    Particle p = sample();
    p.shape = protekt::game::ParticleShape::Icosphere;
    p.velocity = {0.0F, 0.0F, 0.0F};
    p.gravity = 0.0F;
    p.half_to = p.half_from;
    return p;
}

std::vector<SceneVertex> drawn(const Particle& p, protekt::sim::Tick age,
                               const protekt::math::Vec3& eye = kFarEye) {
    std::vector<SceneVertex> out;
    emit_particle(out, p, age, eye);
    return out;
}

std::array<float, 3> centre_of(const std::vector<SceneVertex>& v) {
    std::array<float, 3> lo{1e9F, 1e9F, 1e9F};
    std::array<float, 3> hi{-1e9F, -1e9F, -1e9F};
    for (const SceneVertex& vertex : v) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            lo.at(axis) = std::min(lo.at(axis), vertex.position.at(axis));
            hi.at(axis) = std::max(hi.at(axis), vertex.position.at(axis));
        }
    }
    return {(lo[0] + hi[0]) * 0.5F, (lo[1] + hi[1]) * 0.5F, (lo[2] + hi[2]) * 0.5F};
}

float height_of(const std::vector<SceneVertex>& v) {
    float lo = 1e9F;
    float hi = -1e9F;
    for (const SceneVertex& vertex : v) {
        lo = std::min(lo, vertex.position.at(1));
        hi = std::max(hi, vertex.position.at(1));
    }
    return hi - lo;
}

TEST(Particle, StandsAtItsOriginPlusVelocityTimesAgeLessGravity) {
    const auto at = centre_of(drawn(sample(), 4));
    EXPECT_NEAR(at[0], 14.0F, 1e-3F);
    EXPECT_NEAR(at[1], 5.0F + 2.0F - (0.5F * 0.1F * 16.0F), 1e-3F);
    EXPECT_NEAR(at[2], 10.0F, 1e-3F);
}

TEST(Particle, SizeColourAndAlphaRunFromTheFirstValueToTheLast) {
    const auto start = drawn(sample(), 0);
    ASSERT_FALSE(start.empty());
    EXPECT_NEAR(height_of(start), 2.0F, 1e-3F);
    EXPECT_NEAR(start.front().colour.at(0), 1.0F, 1e-4F);
    EXPECT_NEAR(start.front().alpha, 0.8F, 1e-4F);
    const auto half = drawn(sample(), 5);
    EXPECT_NEAR(height_of(half), 1.5F, 1e-3F);
    EXPECT_NEAR(half.front().colour.at(0), 0.5F, 1e-4F);
    EXPECT_NEAR(half.front().colour.at(2), 0.5F, 1e-4F);
    EXPECT_NEAR(half.front().alpha, 0.4F, 1e-4F);
}

TEST(Particle, IsGoneAtItsLifetimeAndNotBefore) {
    EXPECT_FALSE(drawn(sample(), 9).empty());
    EXPECT_TRUE(drawn(sample(), 10).empty());
}

TEST(Particle, ItsSidesSetAPrismsVertexCount) {
    Particle p = sample();
    EXPECT_EQ(drawn(p, 0).size(), protekt::game::kPuffVertices);
    p.sides = protekt::game::kSparkSides;
    EXPECT_EQ(drawn(p, 0).size(), protekt::game::kSparkVertices);
}

std::array<float, 3> mean_of(const std::vector<SceneVertex>& v) {
    std::array<float, 3> sum{};
    for (const SceneVertex& vertex : v) {
        for (std::size_t axis = 0; axis < 3; ++axis) {
            sum.at(axis) += vertex.position.at(axis);
        }
    }
    for (float& c : sum) {
        c /= static_cast<float>(v.size());
    }
    return sum;
}

float distance_between(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::sqrt(((a[0] - b[0]) * (a[0] - b[0])) + ((a[1] - b[1]) * (a[1] - b[1]))
                     + ((a[2] - b[2]) * (a[2] - b[2])));
}

/// Which way the first vertex points from the centre: where a puff is turned to.
std::array<float, 3> facing_of(const std::vector<SceneVertex>& v) {
    const std::array<float, 3> centre = mean_of(v);
    std::array<float, 3> d{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
        d.at(axis) = v.front().position.at(axis) - centre.at(axis);
    }
    const float length = distance_between(d, {0.0F, 0.0F, 0.0F});
    return {d[0] / length, d[1] / length, d[2] / length};
}

TEST(Particle, AnIcosphereIsTwentyEqualTrianglesOfTwelveCornersOneRadiusOut) {
    // The shape Harri asked for: an ICOSPHERE, uniform equilateral triangles, not a UV sphere.
    const std::vector<SceneVertex> v = drawn(puff(), 0);
    ASSERT_EQ(v.size(), protekt::game::kSphereVertices);
    EXPECT_EQ(v.size(), std::size_t{60});
    const std::array<float, 3> centre = mean_of(v);
    EXPECT_NEAR(centre[0], 10.0F, 1e-3F);
    EXPECT_NEAR(centre[1], 5.0F, 1e-3F);
    EXPECT_NEAR(centre[2], 10.0F, 1e-3F);
    std::set<std::array<long, 3>> corners;
    std::map<std::array<long, 3>, int> triangles_at;
    for (const SceneVertex& vertex : v) {
        EXPECT_NEAR(distance_between(vertex.position, centre),
                    protekt::game::kPuffRadiusPerHalf * 1.0F, 1e-3F);
        const std::array<long, 3> key{std::lround(vertex.position[0] * 1000.0F),
                                      std::lround(vertex.position[1] * 1000.0F),
                                      std::lround(vertex.position[2] * 1000.0F)};
        corners.insert(key);
        ++triangles_at[key];
    }
    EXPECT_EQ(corners.size(), std::size_t{12});
    for (const auto& [corner, count] : triangles_at) {
        EXPECT_EQ(count, 5) << "a UV sphere's pole gathers many triangles; an icosphere's never do";
    }
    for (std::size_t tri = 0; tri < v.size(); tri += 3) {
        const float ab = distance_between(v.at(tri).position, v.at(tri + 1).position);
        const float bc = distance_between(v.at(tri + 1).position, v.at(tri + 2).position);
        const float ca = distance_between(v.at(tri + 2).position, v.at(tri).position);
        EXPECT_NEAR(ab, bc, ab * 0.01F);
        EXPECT_NEAR(bc, ca, bc * 0.01F);
    }
}

TEST(Particle, TwoPuffsAtOneSpotWithDifferentRollsAreTurnedDifferently) {
    Particle a = puff();
    Particle b = puff();
    a.roll = 0.3F;
    b.roll = 1.9F;
    EXPECT_GT(distance_between(facing_of(drawn(a, 0)), facing_of(drawn(b, 0))), 0.05F)
        << "overlapping puffs share one orientation, which is what makes them look identical";
    // The control: the same roll is the same picture, so the difference above is the roll's.
    Particle c = puff();
    c.roll = 0.3F;
    EXPECT_LT(distance_between(facing_of(drawn(a, 0)), facing_of(drawn(c, 0))), 1e-4F);
    // And the centre and size do not move with the roll.
    EXPECT_LT(distance_between(mean_of(drawn(a, 0)), mean_of(drawn(b, 0))), 1e-3F);
}

TEST(Particle, AnIcospheresRollRunsOnWithItsAge) {
    Particle p = puff();
    p.roll = 0.5F;
    p.roll_rate = 0.05F;
    EXPECT_GT(distance_between(facing_of(drawn(p, 0)), facing_of(drawn(p, 6))), 0.05F);
    p.roll_rate = 0.0F;
    EXPECT_LT(distance_between(facing_of(drawn(p, 0)), facing_of(drawn(p, 6))), 1e-4F);
}

TEST(Particle, APrismDoesNotRoll) {
    Particle p = sample();
    p.velocity = {0.0F, 0.0F, 0.0F};
    p.gravity = 0.0F;
    p.roll = 1.0F;
    p.roll_rate = 0.2F;
    Particle q = p;
    q.roll = 0.0F;
    q.roll_rate = 0.0F;
    const auto turned = drawn(p, 3);
    const auto still = drawn(q, 3);
    ASSERT_EQ(turned.size(), still.size());
    for (std::size_t i = 0; i < turned.size(); ++i) {
        EXPECT_EQ(turned.at(i).position, still.at(i).position)
            << "a spark is a cube; it keeps its faces";
    }
}

TEST(Particle, AFloorPerMetreKeepsItAtLeastThatWideWhateverItsSize) {
    Particle p = sample();
    p.half_from = 0.001F;
    p.half_to = 0.001F;
    p.floor_per_metre = 0.01F;
    const protekt::math::Vec3 eye{10.0F, 5.0F, 110.0F}; // 100 m from the origin, at age 0
    EXPECT_NEAR(height_of(drawn(p, 0, eye)), 2.0F * 0.01F * 100.0F, 0.02F);
    // Close up the particle's own size wins.
    EXPECT_NEAR(height_of(drawn(p, 0, {10.0F, 5.0F, 10.05F})), 0.002F, 1e-4F);
}

TEST(Spark, IsOpaqueAndBrightAtBothEndsOfItsLife) {
    for (const auto& ink : {protekt::game::kSparkHotColour, protekt::game::kSparkCoolColour}) {
        const float luma = (0.2126F * ink[0]) + (0.7152F * ink[1]) + (0.0722F * ink[2]);
        EXPECT_GE(luma, 0.75F) << "a spark is the brightest thing on the screen";
        EXPECT_GE(ink[0], 0.99F) << "and it is saturated hot, not a mid grey";
    }
}

TEST(Spark, ItsPixelFloorIsTwoAndAHalfRowsAtTheReferenceHeight) {
    // Half-width per metre of distance for 2.5 px at 720 rows and a 60 degree view.
    const float per_pixel = 2.0F * std::tan(protekt::math::kFovRadians * 0.5F)
                            / static_cast<float>(protekt::game::kSparkReferenceRows);
    EXPECT_NEAR(protekt::game::kSparkFloorPerMetre,
                0.5F * protekt::game::kSparkMinPixels * per_pixel, 1e-5F);
    EXPECT_GE(protekt::game::kSparkMinPixels, 2.0F);
    EXPECT_LE(protekt::game::kSparkMinPixels, 3.0F);
}

} // namespace
