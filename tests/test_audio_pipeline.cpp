// The step-and-listen half of a frame: advance the sim N ticks, derive what the player hears from
// the states either side of each one, and hand ONE mix to the device. Sound never touches the
// simulation, so the strongest thing to pin is that stepping through the pipeline is stepping.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "protekt/app/audio_pipeline.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/sim/orders.hpp"
#include "protekt/sim/scenario.hpp"
#include "protekt/sim/sim.hpp"

namespace {

using protekt::app::AudioFrame;
using protekt::app::AudioPipeline;
using protekt::app::SampleSink;

class FakeSink final : public SampleSink {
public:
    bool healthy{true};
    int plays{0};
    std::size_t last_samples{0};

    [[nodiscard]] bool ok() const noexcept override { return healthy; }
    void play(std::span<const float> samples, int /*sample_rate*/) noexcept override {
        ++plays;
        last_samples = samples.size();
    }
};

protekt::sim::Sim make_sim() { return protekt::sim::Sim::create(protekt::sim::skirmish(7)); }

AudioFrame frame_for(bool shot_fired) {
    return AudioFrame{protekt::game::Listener{}, shot_fired, true, true};
}

TEST(AudioPipeline, StepsExactlyTheTicksAskedFor) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    AudioPipeline pipeline{nullptr};
    pipeline.advance(sim, 5, frame_for(false), flash, mark);
    EXPECT_EQ(sim.tick(), 5U);
}

TEST(AudioPipeline, ListeningDoesNotMoveTheSimulation) {
    protekt::sim::Sim heard = make_sim();
    protekt::sim::Sim silent = make_sim();
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    FakeSink sink;
    AudioPipeline listening{&sink};
    listening.advance(heard, 40, frame_for(true), flash, mark);
    for (int i = 0; i < 40; ++i) {
        silent.step();
    }
    EXPECT_EQ(heard.state_hash(), silent.state_hash());
}

/// The first player tank, with its gun loaded: a skirmish's tanks start reloading.
protekt::sim::UnitId loaded_tank(protekt::sim::Sim& sim) {
    protekt::sim::UnitId tank{};
    for (const protekt::sim::Unit& unit : sim.units()) {
        if (unit.side == protekt::sim::Side::Player && unit.kind == protekt::sim::UnitKind::Tank) {
            tank = unit.id;
            break;
        }
    }
    for (int i = 0; i < 20; ++i) {
        sim.step();
    }
    return tank;
}

AudioFrame trigger_for(protekt::sim::UnitId player) {
    return AudioFrame{protekt::game::Listener{}, true, true, true, player};
}

// M5 re-record: this was "a shot at the ground is one mix" with NO shot in the sim. The sound now
// plays if and only if the sim launched the shot, so the shot is made for real.
TEST(AudioPipeline, ALaunchedShotIsOneMixHandedToTheDevice) {
    protekt::sim::Sim sim = make_sim();
    const protekt::sim::UnitId tank = loaded_tank(sim);
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    FakeSink sink;
    AudioPipeline pipeline{&sink};
    sim.enqueue(protekt::sim::Order{
        tank, protekt::sim::orders::FireTowards{protekt::sim::Fixed::from_int(1),
                                                protekt::sim::Fixed::zero(), protekt::sim::UnitId{},
                                                protekt::sim::Fixed::zero()}});
    const protekt::app::AdvanceResult result
        = pipeline.advance(sim, 3, trigger_for(tank), flash, mark);
    EXPECT_FALSE(result.trigger_refused);
    EXPECT_EQ(sink.plays, 1);
    EXPECT_GT(sink.last_samples, 0U);
}

TEST(AudioPipeline, ATriggerTheSimDeclinedMakesNoSoundAndIsReported) {
    protekt::sim::Sim sim = make_sim();
    const protekt::sim::UnitId tank = loaded_tank(sim);
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    FakeSink sink;
    AudioPipeline pipeline{&sink};
    const protekt::app::AdvanceResult result
        = pipeline.advance(sim, 3, trigger_for(tank), flash, mark);
    EXPECT_TRUE(result.trigger_refused) << "no order was queued, so no shot was launched";
    EXPECT_EQ(sink.plays, 0);
}

TEST(AudioPipeline, SilenceIsNotPlayed) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    FakeSink sink;
    AudioPipeline pipeline{&sink};
    pipeline.advance(sim, 1, frame_for(false), flash, mark);
    EXPECT_EQ(sink.plays, 0);
}

TEST(AudioPipeline, AFailedDeviceIsNeverPlayedTo) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    protekt::game::HitMark mark;
    FakeSink sink;
    sink.healthy = false;
    AudioPipeline pipeline{&sink};
    pipeline.advance(sim, 3, frame_for(true), flash, mark);
    EXPECT_EQ(sink.plays, 0);
    EXPECT_EQ(sim.tick(), 3U);
}

} // namespace
