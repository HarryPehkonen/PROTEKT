#include "protekt/app/audio_pipeline.hpp"

#include <vector>

#include "protekt/audio/mixer.hpp"
#include "protekt/game/interaction.hpp"
#include "protekt/sim/observer.hpp"

namespace protekt::app {

namespace {

/// What the player's hit points were before a tick: the living units' `{id, hp}`. Taken from the
/// snapshot the events are diffed against, so the two cannot describe different moments.
std::vector<game::Health> health_of(const sim::UnitSnapshot& snapshot) {
    std::vector<game::Health> health;
    health.reserve(snapshot.size());
    for (const sim::Unit& unit : snapshot) {
        if (unit.alive) {
            health.push_back(game::Health{unit.id, unit.hp});
        }
    }
    return health;
}

} // namespace

void AudioPipeline::advance(sim::Sim& sim, std::uint32_t steps, const AudioFrame& frame,
                            game::HitFlash& hit_flash) const {
    std::vector<audio::Voice> voices;
    if (frame.shot_fired) {
        voices.push_back(game::shot_voice());
    }
    const bool listening = sink_ != nullptr && sink_->ok() && frame.player_alive_now;
    for (std::uint32_t tick = 0; tick < steps; ++tick) {
        if (!listening && !frame.player_alive_before) {
            sim.step();
            continue;
        }
        const sim::UnitSnapshot before = sim.units();
        const sim::Tick tick_number = sim.tick();
        sim.step();
        const std::vector<sim::BattleEvent> events = sim::events_between(before, sim, tick_number);
        if (game::unit_was_hit(events, frame.player)) {
            hit_flash.hit(sim.tick());
        }
        if (!listening) {
            continue;
        }
        const std::vector<game::Health> health = health_of(before);
        const std::vector<audio::Voice> heard
            = game::sounds_for(events, frame.listener, before, health);
        voices.insert(voices.end(), heard.begin(), heard.end());
    }
    if (!voices.empty() && sink_ != nullptr && sink_->ok()) {
        const std::vector<float> samples
            = audio::mix(voices, audio::frames_to_play(voices), audio::kSampleRate);
        sink_->play(samples, audio::kSampleRate);
    }
}

} // namespace protekt::app
