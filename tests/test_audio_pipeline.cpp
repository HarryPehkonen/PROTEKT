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
    AudioPipeline pipeline{nullptr};
    pipeline.advance(sim, 5, frame_for(false), flash);
    EXPECT_EQ(sim.tick(), 5U);
}

TEST(AudioPipeline, ListeningDoesNotMoveTheSimulation) {
    protekt::sim::Sim heard = make_sim();
    protekt::sim::Sim silent = make_sim();
    protekt::game::HitFlash flash;
    FakeSink sink;
    AudioPipeline listening{&sink};
    listening.advance(heard, 40, frame_for(true), flash);
    for (int i = 0; i < 40; ++i) {
        silent.step();
    }
    EXPECT_EQ(heard.state_hash(), silent.state_hash());
}

TEST(AudioPipeline, AShotAtTheGroundIsOneMixHandedToTheDevice) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    FakeSink sink;
    AudioPipeline pipeline{&sink};
    pipeline.advance(sim, 3, frame_for(true), flash);
    EXPECT_EQ(sink.plays, 1);
    EXPECT_GT(sink.last_samples, 0U);
}

TEST(AudioPipeline, SilenceIsNotPlayed) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    FakeSink sink;
    AudioPipeline pipeline{&sink};
    pipeline.advance(sim, 1, frame_for(false), flash);
    EXPECT_EQ(sink.plays, 0);
}

TEST(AudioPipeline, AFailedDeviceIsNeverPlayedTo) {
    protekt::sim::Sim sim = make_sim();
    protekt::game::HitFlash flash;
    FakeSink sink;
    sink.healthy = false;
    AudioPipeline pipeline{&sink};
    pipeline.advance(sim, 3, frame_for(true), flash);
    EXPECT_EQ(sink.plays, 0);
    EXPECT_EQ(sim.tick(), 3U);
}

} // namespace
