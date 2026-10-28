// The wake of a kill has to outlive the frame that saw the kill. The scene derives a wreck's burst
// and its 14-second column from `now - event.tick`, so the event that drives it must still be in
// the frame's options long after the frame that stepped the kill. Every test here goes through the
// REAL step: `AudioPipeline::advance` over a real sim, then the same `WakeLog::after_frame` call
// the game makes -- a hand-built events span is exactly what let a one-frame event pass the earlier
// tests while the real game drew nothing.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <vector>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

namespace sim = protekt::sim;
namespace game = protekt::game;

using protekt::app::AudioFrame;
using protekt::app::AudioPipeline;

/// An armed player tank with an unarmed enemy hauler four tiles off: the tank's own gun kills it,
/// with no order given, the same arrangement the scene's wake tests use.
[[nodiscard]] sim::Sim a_hostile_in_reach() {
    sim::Scenario scenario;
    scenario.seed = 7ULL;
    scenario.spawns = {{sim::UnitKind::Tank, sim::Side::Player, 40, 40},
                       {sim::UnitKind::Scavenger, sim::Side::Enemy, 44, 40}};
    return sim::Sim::create(scenario);
}

/// One frame the way `Game::advance` runs it: snapshot, the pipeline's step, then the log.
void run_frame(sim::Sim& sim, game::WakeLog& wakes) {
    static const AudioPipeline pipeline{nullptr};
    game::HitFlash flash;
    game::HitMark mark;
    const sim::UnitSnapshot before = sim.units();
    const sim::Tick stamp = sim.tick();
    pipeline.advance(sim, 1, AudioFrame{}, flash, mark);
    wakes.after_frame(before, sim, stamp);
}

[[nodiscard]] std::size_t kills_of(const game::WakeLog& wakes, sim::UnitId victim) {
    const auto events = wakes.events();
    return static_cast<std::size_t>(
        std::count_if(events.begin(), events.end(), [&](const sim::BattleEvent& event) {
            return event.kind == sim::EventKind::Killed && event.subject == victim;
        }));
}

/// Steps until a frame reports a kill; returns the victim, or an invalid id if none came.
[[nodiscard]] sim::UnitId run_to_the_kill(sim::Sim& sim, game::WakeLog& wakes) {
    for (int frame = 0; frame < 4000; ++frame) {
        run_frame(sim, wakes);
        for (const sim::BattleEvent& event : wakes.events()) {
            if (event.kind == sim::EventKind::Killed) {
                return event.subject;
            }
        }
    }
    return sim::UnitId{};
}

TEST(WakeLog, AKillStillBurnsOnTheFramesAfterTheOneThatSawIt) {
    sim::Sim sim = a_hostile_in_reach();
    game::WakeLog wakes;
    const sim::UnitId victim = run_to_the_kill(sim, wakes);
    ASSERT_TRUE(victim.valid()) << "the arrangement never produced a kill";
    for (int frame = 0; frame < 100; ++frame) {
        run_frame(sim, wakes);
        EXPECT_EQ(kills_of(wakes, victim), 1U)
            << "frame " << frame << " after the kill lost the event: the wake would vanish";
    }
}

TEST(WakeLog, TheRetainedKillDrawsAColumnOnALaterFrameOfTheRealGame) {
    sim::Sim sim = a_hostile_in_reach();
    game::WakeLog wakes;
    ASSERT_TRUE(run_to_the_kill(sim, wakes).valid());
    for (int frame = 0; frame < 100; ++frame) {
        run_frame(sim, wakes);
    }
    const sim::Unit* dead = [&]() -> const sim::Unit* {
        for (const sim::Unit& unit : sim.units()) {
            if (!unit.alive) {
                return &unit;
            }
        }
        return nullptr;
    }();
    ASSERT_NE(dead, nullptr);
    game::SceneOptions plain;
    plain.seed = 7ULL;
    plain.camera.x = static_cast<float>(dead->x.floor_int()) * game::kMetresPerTile;
    plain.camera.z = static_cast<float>(dead->y.floor_int()) * game::kMetresPerTile;
    plain.camera.y = 8.0F;
    plain.show_ship_gauge = false;
    plain.show_ledger = false;
    game::SceneOptions with = plain;
    with.events = wakes.events();
    EXPECT_EQ(game::build_scene(sim, with).vertices.size()
                  - game::build_scene(sim, plain).vertices.size(),
              game::kSmokePuffs * std::size_t{36})
        << "a hundred ticks after the kill the wreck should be carrying its whole column";
}

TEST(WakeLog, AKillIsForgottenOnceTheSmokeHasRunOut) {
    sim::Sim sim = a_hostile_in_reach();
    game::WakeLog wakes;
    const sim::UnitId victim = run_to_the_kill(sim, wakes);
    ASSERT_TRUE(victim.valid());
    // The event is stamped with the tick before the blow, and the scene calls an age of
    // kSmokeTicks cold; both ends are the sim's own counter, no second clock.
    const sim::Tick stamp = sim.tick() - 1;
    while (sim.tick() - stamp < game::kSmokeTicks - 1) {
        run_frame(sim, wakes);
        ASSERT_EQ(kills_of(wakes, victim), 1U) << "forgotten early, at tick " << sim.tick();
    }
    run_frame(sim, wakes);
    EXPECT_EQ(sim.tick() - stamp, game::kSmokeTicks);
    EXPECT_EQ(kills_of(wakes, victim), 0U) << "a wreck would smoke for ever";
    EXPECT_TRUE(wakes.events().empty());
}

TEST(WakeLog, TheSameKillSeenTwiceIsListedOnce) {
    sim::Sim sim = a_hostile_in_reach();
    game::WakeLog wakes;
    const sim::UnitId victim = run_to_the_kill(sim, wakes);
    ASSERT_TRUE(victim.valid());
    // A frame that steps nothing re-reads the same impacts: the sim's last tick is still the kill.
    const sim::UnitSnapshot before = sim.units();
    wakes.after_frame(before, sim, sim.tick() - 1);
    wakes.after_frame(before, sim, sim.tick() - 1);
    EXPECT_EQ(kills_of(wakes, victim), 1U)
        << "listed twice, the column would be drawn twice and read twice as dense";
}

TEST(WakeLog, OnlyKillsAreKept) {
    sim::Sim sim = a_hostile_in_reach();
    game::WakeLog wakes;
    for (int frame = 0; frame < 4000; ++frame) {
        run_frame(sim, wakes);
        for (const sim::BattleEvent& event : wakes.events()) {
            EXPECT_EQ(event.kind, sim::EventKind::Killed)
                << "the hit flash and the hit mark read HitTaken from the frame, not from here";
        }
    }
}

} // namespace
