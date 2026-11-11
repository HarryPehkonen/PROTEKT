// Round 15, slice F: the Harvester's six legs walk. The gait is DRAWING: a walk phase that advances
// with the distance a machine actually moved (`Strides`), and a tripod of legs moved by it. The sim
// is never touched, and a frame that never heard of `Strides` is unchanged.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include "protekt/game/faction.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::game {
namespace {

using sim::Side;
using sim::Spawn;
using sim::UnitKind;

constexpr std::uint64_t kSeed = 42;
constexpr int kTile = 60;
constexpr Factions kAlienPlays{Faction::Alien, Faction::Alien};
/// How far the box may move: measured at 2.2 cm across at the worst phase (the outer edge of a
/// foot's ring tilts with the leg's axis as the knee swings), on a machine 5.45 m by 2.2 m.
constexpr float kExtentSlack = 0.03F;

[[nodiscard]] sim::Sim lone_harvester() {
    return sim::Sim::create(sim::Scenario{
        kSeed, std::vector<Spawn>{{UnitKind::Scavenger, Side::Player, kTile, kTile}}});
}

/// The snapshot of the same sim, the unit `metres` back along +x: "it moved that far this frame".
[[nodiscard]] sim::UnitSnapshot moved_from(const sim::Sim& sim, float metres) {
    sim::UnitSnapshot before = sim.units();
    before.front().x.raw -= static_cast<std::int32_t>(metres / kMetresPerTile
                                                      * static_cast<float>(sim::Fixed::kOne));
    return before;
}

/// The frame of `sim`, with the camera on `unit` (which an empty world does not have).
[[nodiscard]] std::vector<SceneVertex> frame(const sim::Sim& sim, const Strides* strides,
                                             const sim::Unit& unit) {
    SceneOptions options;
    options.seed = kSeed;
    options.radius_tiles = 8;
    options.show_ship_gauge = false;
    options.show_ledger = false;
    options.factions = kAlienPlays;
    options.strides = strides;
    options.camera.x
        = static_cast<float>(unit.x.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile;
    options.camera.z
        = static_cast<float>(unit.y.raw) / static_cast<float>(sim::Fixed::kOne) * kMetresPerTile
          - 8.0F;
    options.camera.y = ground_metres(kSeed, unit.x, unit.y) + 5.0F;
    options.camera.pitch = -0.25F;
    return build_scene(sim, options).vertices;
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

struct Box {
    std::array<float, 3> low{1e9F, 1e9F, 1e9F};
    std::array<float, 3> high{-1e9F, -1e9F, -1e9F};
};

/// The box of the vertices the unit adds to the frame (it is the tail of the terrain-and-sky both
/// frames share; the HUD adds the same to both, so the diff against an empty world finds it).
[[nodiscard]] Box machine_box(const sim::Sim& sim, const Strides* strides) {
    const std::vector<SceneVertex> with = frame(sim, strides, sim.units().front());
    sim::Sim empty = sim::Sim::create(sim::Scenario{kSeed, {}});
    const std::vector<SceneVertex> base = frame(empty, nullptr, sim.units().front());
    Box box;
    std::size_t head = 0;
    while (head < base.size() && head < with.size()
           && base.at(head).position == with.at(head).position) {
        ++head;
    }
    std::size_t tail = 0;
    while (tail < base.size() - head && tail < with.size() - head
           && base.at(base.size() - 1 - tail).position
                  == with.at(with.size() - 1 - tail).position) {
        ++tail;
    }
    for (std::size_t i = head; i + tail < with.size(); ++i) {
        for (std::size_t k = 0; k < 3; ++k) {
            box.low.at(k) = std::min(box.low.at(k), with.at(i).position.at(k));
            box.high.at(k) = std::max(box.high.at(k), with.at(i).position.at(k));
        }
    }
    return box;
}

[[nodiscard]] Strides walked(const sim::Sim& sim, float metres) {
    Strides strides;
    strides.after_frame(moved_from(sim, metres), sim);
    return strides;
}

TEST(HarvesterGait, AParkedMachineEmitsTheSameLegsOnConsecutiveFrames) {
    const sim::Sim sim = lone_harvester();
    Strides strides;
    strides.after_frame(sim.units(), sim);
    const std::vector<SceneVertex> first = frame(sim, &strides, sim.units().front());
    strides.after_frame(sim.units(), sim);
    const std::vector<SceneVertex> second = frame(sim, &strides, sim.units().front());
    EXPECT_EQ(differing(first, second), 0U);
    EXPECT_FALSE(strides.of(sim.units().front()).has_value()) << "never moved: no phase";
    // And the neutral stance is what a frame that never heard of strides draws.
    EXPECT_EQ(differing(first, frame(sim, nullptr, sim.units().front())), 0U);
}

TEST(HarvesterGait, AMachineThatStopsHoldsItsLegsExactly) {
    const sim::Sim sim = lone_harvester();
    Strides strides = walked(sim, 0.5F);
    const float held = strides.of(sim.units().front()).value_or(-1.0F);
    ASSERT_GE(held, 0.0F) << "it moved, so it has a phase";
    const std::vector<SceneVertex> before = frame(sim, &strides, sim.units().front());
    strides.after_frame(sim.units(), sim); // a frame in which it moved nowhere
    EXPECT_EQ(strides.of(sim.units().front()).value_or(-2.0F), held);
    EXPECT_EQ(differing(before, frame(sim, &strides, sim.units().front())), 0U);
}

TEST(HarvesterGait, AMovingMachinesLegsSwingBetweenPhases) {
    const sim::Sim sim = lone_harvester();
    const Strides early = walked(sim, 0.35F);
    const Strides late = walked(sim, 0.70F);
    const std::vector<SceneVertex> a = frame(sim, &early, sim.units().front());
    const std::vector<SceneVertex> b = frame(sim, &late, sim.units().front());
    EXPECT_GT(differing(a, b), 100U) << "six legs moved";
    // Only the legs: the carapace, the tail, the comb, the mandibles and the eyes stay put, so a
    // large part of the machine is bit-for-bit the same.
    EXPECT_LT(differing(a, b), a.size() / 2U);
}

TEST(HarvesterGait, PhaseFollowsDistanceNotTheNumberOfFrames) {
    const sim::Sim sim = lone_harvester();
    // 0.7 m in one frame is 0.35 m twice: the same phase, and so the same legs.
    Strides twice;
    twice.after_frame(moved_from(sim, 0.35F), sim);
    twice.after_frame(moved_from(sim, 0.35F), sim);
    const Strides once = walked(sim, 0.70F);
    EXPECT_NEAR(twice.of(sim.units().front()).value_or(-1.0F),
                once.of(sim.units().front()).value_or(-2.0F), 1e-3F);
}

TEST(HarvesterGait, TheTwoTripodsAreNeverBothPlanted) {
    constexpr int kSamples = 200;
    for (int i = 0; i < kSamples; ++i) {
        const float phase = (static_cast<float>(i) + 0.37F) / static_cast<float>(kSamples);
        for (const std::size_t leg : {std::size_t{0}, std::size_t{2}, std::size_t{4}}) {
            EXPECT_EQ(leg_pose(phase, leg).along, leg_pose(phase, 0).along)
                << "legs 1-3-5 together";
            EXPECT_EQ(leg_pose(phase, leg).lift, leg_pose(phase, 0).lift);
            EXPECT_EQ(leg_pose(phase, leg + 1).lift, leg_pose(phase, 1).lift) << "2-4-6 together";
        }
        const bool a_planted = leg_pose(phase, 0).lift == 0.0F;
        const bool b_planted = leg_pose(phase, 1).lift == 0.0F;
        EXPECT_NE(a_planted, b_planted) << "one set planted while the other swings, at " << phase;
    }
}

TEST(HarvesterGait, APlantedFootStaysPutWhileTheBodyAdvances) {
    // Stance: the foot slides aft by exactly the ground the body covered, so it is fixed to the
    // world.
    for (const std::size_t leg : {std::size_t{0}, std::size_t{1}}) {
        int planted_steps = 0;
        for (int i = 0; i < 200; ++i) {
            const float phase = (static_cast<float>(i) + 0.2F) / 200.0F;
            const float next = phase + 0.001F;
            const LegPose now = leg_pose(phase, leg);
            const LegPose later = leg_pose(next, leg);
            if (now.lift == 0.0F && later.lift == 0.0F) {
                ++planted_steps;
                const float body = (next - phase) * kStrideMetres;
                EXPECT_NEAR(later.along - now.along, -body, 1e-4F) << "leg " << leg << " skated";
            }
        }
        EXPECT_GT(planted_steps, 80) << "a leg is planted for about half the cycle";
    }
}

TEST(HarvesterGait, TheFootNeverReachesPastItsStrideOrTheGround) {
    for (int i = 0; i < 400; ++i) {
        const LegPose pose
            = leg_pose(static_cast<float>(i) / 400.0F, static_cast<std::size_t>(i % 6));
        EXPECT_LE(std::abs(pose.along), kStrideHalfMetres + 1e-5F);
        EXPECT_GE(pose.lift, 0.0F);
        EXPECT_LE(pose.lift, kStepLiftMetres + 1e-5F);
    }
}

TEST(HarvesterGait, TheMachineKeepsItsExtentsAndItsVertexCount) {
    const sim::Sim sim = lone_harvester();
    const Box neutral = machine_box(sim, nullptr);
    const std::size_t count = frame(sim, nullptr, sim.units().front()).size();
    for (const float metres : {0.1F, 0.35F, 0.7F, 1.05F, 1.3F}) {
        const Strides strides = walked(sim, metres);
        EXPECT_EQ(frame(sim, &strides, sim.units().front()).size(), count)
            << "the legs are the same geometry, moved";
        const Box box = machine_box(sim, &strides);
        for (std::size_t k = 0; k < 3; ++k) {
            EXPECT_NEAR(box.low.at(k), neutral.low.at(k), kExtentSlack)
                << "axis " << k << " at " << metres;
            EXPECT_NEAR(box.high.at(k), neutral.high.at(k), kExtentSlack)
                << "axis " << k << " at " << metres;
        }
    }
}

TEST(HarvesterGait, TheSameStridesGiveTheSameBytes) {
    const sim::Sim sim = lone_harvester();
    const Strides one = walked(sim, 0.9F);
    const Strides two = walked(sim, 0.9F);
    EXPECT_EQ(
        differing(frame(sim, &one, sim.units().front()), frame(sim, &two, sim.units().front())),
        0U);
}

} // namespace
} // namespace protekt::game
