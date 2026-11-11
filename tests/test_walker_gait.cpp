// Round 15, slice G: the Walker's two legs walk. They had to LEAVE the cached `shape_of(Walker)`
// (a `static const`, built once) to do it: `walker_legs` makes them per pose and the mesh emits
// them before the body. The gait is slice F's (`leg_pose`, `Strides`); a biped is legs 0 and 1,
// half a cycle apart. Drawing only: the sim is never touched.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "protekt/game/scene.hpp"
#include "protekt/game/vehicle_shape.hpp"
#include "protekt/sim/sim.hpp"
#include "untilt.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = 60;
/// Six leg parts, 84 vertices each (a four-cornered, bevelled prism: 24 * 4 - 12), rear leg first.
constexpr std::size_t kLegVertices = 6UL * 84UL;
constexpr std::size_t kOneLeg = 3UL * 84UL;

[[nodiscard]] sim::Sim lone_walker() {
    return sim::Sim::create(
        sim::Scenario{kSeed, std::vector<Spawn>{{UnitKind::Walker, Side::Player, kTile, kTile}}});
}

[[nodiscard]] sim::UnitSnapshot moved_from(const sim::Sim& sim, float metres) {
    sim::UnitSnapshot before = sim.units();
    before.front().x.raw -= static_cast<std::int32_t>(metres / kMetresPerTile
                                                      * static_cast<float>(sim::Fixed::kOne));
    return before;
}

[[nodiscard]] Strides walked(const sim::Sim& sim, float metres) {
    Strides strides;
    strides.after_frame(moved_from(sim, metres), sim);
    return strides;
}

[[nodiscard]] SceneOptions options_for(const sim::Unit& unit, const Strides* strides) {
    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.strides = strides;
    options.camera.x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    options.camera.z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile
          - 8.0F;
    options.camera.y = ground_metres(kSeed, unit.x, unit.y) + 5.0F;
    options.camera.pitch = -0.25F;
    return options;
}

[[nodiscard]] std::vector<SceneVertex> frame(const sim::Sim& sim, const Strides* strides) {
    return build_scene(sim, options_for(sim.units().front(), strides)).vertices;
}

/// The walker's own vertices of a frame: the ones that move when it is hovered far above the
/// ground (the way `tank_only` finds a machine), in emission order -- the legs first.
[[nodiscard]] std::vector<SceneVertex> machine(const sim::Sim& sim, const Strides* strides) {
    SceneOptions options = options_for(sim.units().front(), strides);
    const std::vector<SceneVertex> base = build_scene(sim, options).vertices;
    options.hover = SceneOptions::Hovering{sim.units().front().id, 1000.0F};
    const std::vector<SceneVertex> raised = build_scene(sim, options).vertices;
    std::vector<SceneVertex> out;
    for (std::size_t i = 0; i < base.size() && i < raised.size(); ++i) {
        if (base.at(i).position != raised.at(i).position) {
            out.push_back(base.at(i));
        }
    }
    // The design, not the lean: the body is read off whatever ground the walker stands on.
    untilt::level(out, sim.units().front(), kSeed);
    return out;
}

[[nodiscard]] std::vector<SceneVertex> slice(const std::vector<SceneVertex>& v, std::size_t first,
                                             std::size_t count) {
    return {v.begin() + static_cast<std::ptrdiff_t>(first),
            v.begin() + static_cast<std::ptrdiff_t>(first + count)};
}

[[nodiscard]] std::size_t differing(const std::vector<SceneVertex>& a,
                                    const std::vector<SceneVertex>& b) {
    EXPECT_EQ(a.size(), b.size());
    std::size_t count = 0;
    for (std::size_t i = 0; i < std::min(a.size(), b.size()); ++i) {
        count += a.at(i).position == b.at(i).position ? 0U : 1U;
    }
    return count;
}

[[nodiscard]] float lowest(const std::vector<SceneVertex>& v) {
    float low = 1e9F;
    for (const SceneVertex& vertex : v) {
        low = std::min(low, vertex.position.at(1));
    }
    return low;
}

TEST(WalkerGait, TheLegsAreNoLongerInTheCachedShapeSoNothingCanDrawThemTwice) {
    for (const ShapePart& part : shape_of(UnitKind::Walker)) {
        for (const auto& corner : part.profile) {
            EXPECT_GE(corner[1], 1.69F) << "a part below the hips is a leg, and it walks";
        }
    }
    EXPECT_EQ(walker_legs({}, {}).size(), std::size_t{6}) << "a thigh, a shin and a foot a side";
    EXPECT_EQ(silhouette_of(UnitKind::Walker).size(), shape_of(UnitKind::Walker).size() + 6U)
        << "the plate still sees the legs";
    // And the mesh draws each once: the walker is its legs, its body, and its gun arm.
    const sim::Sim sim = lone_walker();
    EXPECT_EQ(machine(sim, nullptr).size(), kWalkerVertices);
}

TEST(WalkerGait, TheStationaryMachineEmitsByteIdenticalLegsOnConsecutiveTicks) {
    const sim::Sim sim = lone_walker();
    Strides strides;
    strides.after_frame(sim.units(), sim);
    const std::vector<SceneVertex> first = machine(sim, &strides);
    strides.after_frame(sim.units(), sim);
    const std::vector<SceneVertex> second = machine(sim, &strides);
    EXPECT_EQ(differing(slice(first, 0, kLegVertices), slice(second, 0, kLegVertices)), 0U);
    EXPECT_EQ(differing(first, second), 0U);
    EXPECT_EQ(differing(first, machine(sim, nullptr)), 0U)
        << "never moved: the authored stance, as before there was a gait";
}

TEST(WalkerGait, AMachineThatStopsHoldsItsLegsExactly) {
    const sim::Sim sim = lone_walker();
    Strides strides = walked(sim, 0.5F);
    const std::vector<SceneVertex> held = machine(sim, &strides);
    strides.after_frame(sim.units(), sim); // it moved nowhere this tick
    EXPECT_EQ(differing(held, machine(sim, &strides)), 0U);
}

TEST(WalkerGait, AMovingMachinesLegsDifferBetweenPhasesAndTheTwoAreNotInStep) {
    const sim::Sim sim = lone_walker();
    const Strides early = walked(sim, 0.4F);
    const Strides late = walked(sim, 0.8F);
    const std::vector<SceneVertex> a = machine(sim, &early);
    const std::vector<SceneVertex> b = machine(sim, &late);
    EXPECT_GT(differing(slice(a, 0, kLegVertices), slice(b, 0, kLegVertices)), 100U);
    // The body and the gun arm do not walk.
    EXPECT_EQ(differing(slice(a, kLegVertices, a.size() - kLegVertices),
                        slice(b, kLegVertices, b.size() - kLegVertices)),
              0U);
    // A quarter-cycle in (0.4 m of 1.6), one foot is planted on the ground and the other is in the
    // air: the rear leg (first 252 vertices) is the lifted one, the front is the planted one.
    const float rear = lowest(slice(a, 0, kOneLeg));
    const float front = lowest(slice(a, kOneLeg, kOneLeg));
    EXPECT_GT(rear - front, 0.5F * kStepLiftMetres) << "the legs are half a cycle apart";
    EXPECT_NEAR(front, lowest(slice(machine(sim, nullptr), kOneLeg, kOneLeg)), 1e-4F)
        << "the planted foot is on the ground it stood on";
}

TEST(WalkerGait, PhaseFollowsDistanceAndAWalkerCyclesEveryFourReaches) {
    EXPECT_FLOAT_EQ(stride_metres(UnitKind::Walker), 4.0F * kWalkerStrideHalfMetres);
    const sim::Sim sim = lone_walker();
    Strides twice;
    twice.after_frame(moved_from(sim, 0.4F), sim);
    twice.after_frame(moved_from(sim, 0.4F), sim);
    EXPECT_NEAR(twice.of(sim.units().front()).value_or(-1.0F),
                walked(sim, 0.8F).of(sim.units().front()).value_or(-2.0F), 1e-3F);
    EXPECT_NEAR(walked(sim, 0.4F).of(sim.units().front()).value_or(-1.0F), 0.25F, 1e-3F);
}

TEST(WalkerGait, ExactlyOneFootIsPlantedAndItNeverSkates) {
    constexpr float kH = kWalkerStrideHalfMetres;
    for (int i = 0; i < 200; ++i) {
        const float phase = (static_cast<float>(i) + 0.37F) / 200.0F;
        EXPECT_NE(leg_pose(phase, 0, kH).lift == 0.0F, leg_pose(phase, 1, kH).lift == 0.0F)
            << "one planted, one swinging, at " << phase;
        for (const std::size_t leg : {std::size_t{0}, std::size_t{1}}) {
            const float next = phase + 0.001F;
            const LegPose now = leg_pose(phase, leg, kH);
            const LegPose later = leg_pose(next, leg, kH);
            EXPECT_LE(std::abs(now.along), kH + 1e-5F);
            if (now.lift == 0.0F && later.lift == 0.0F) {
                EXPECT_NEAR(later.along - now.along,
                            -(next - phase) * stride_metres(UnitKind::Walker), 1e-4F)
                    << "leg " << leg << " skated";
            }
        }
    }
}

TEST(WalkerGait, TheMachinesExtentsHoldAtEveryPhaseAndTheCountDoesNot) {
    const sim::Sim sim = lone_walker();
    const std::vector<SceneVertex> neutral = machine(sim, nullptr);
    const auto box = [](const std::vector<SceneVertex>& v) {
        std::array<float, 6> b{1e9F, 1e9F, 1e9F, -1e9F, -1e9F, -1e9F};
        for (const SceneVertex& vertex : v) {
            for (std::size_t k = 0; k < 3; ++k) {
                b.at(k) = std::min(b.at(k), vertex.position.at(k));
                b.at(k + 3) = std::max(b.at(k + 3), vertex.position.at(k));
            }
        }
        return b;
    };
    const auto base = box(neutral);
    for (const float metres : {0.1F, 0.4F, 0.8F, 1.2F, 1.5F}) {
        const Strides strides = walked(sim, metres);
        const std::vector<SceneVertex> now = machine(sim, &strides);
        EXPECT_EQ(now.size(), neutral.size());
        const auto b = box(now);
        // A planted foot ends where a static foot ends: nothing reaches farther out than the
        // authored stance did. (Turned along the heading, so the test is on all three axes.)
        for (std::size_t k = 0; k < 3; ++k) {
            EXPECT_GE(b.at(k), base.at(k) - 0.001F) << "axis " << k << " at " << metres;
            EXPECT_LE(b.at(k + 3), base.at(k + 3) + 0.001F) << "axis " << k << " at " << metres;
        }
    }
}

TEST(WalkerGait, TheSameStridesGiveTheSameBytes) {
    const sim::Sim sim = lone_walker();
    const Strides one = walked(sim, 0.9F);
    const Strides two = walked(sim, 0.9F);
    EXPECT_EQ(differing(frame(sim, &one), frame(sim, &two)), 0U);
}

} // namespace
} // namespace protekt::game
