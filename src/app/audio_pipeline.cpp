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

AdvanceResult AudioPipeline::advance(sim::Sim& sim, std::uint32_t steps, const AudioFrame& frame,
                                     game::HitFlash& hit_flash, game::HitMark& hit_mark) const {
    AdvanceResult result;
    std::vector<audio::Voice> voices;
    // The trigger is judged on the first step: that is the one that applies the order it queued.
    bool judged = !frame.trigger_pulled;
    const bool listening = sink_ != nullptr && sink_->ok() && frame.player_alive_now;
    for (std::uint32_t tick = 0; tick < steps; ++tick) {
        const bool observing = listening || frame.player_alive_before;
        sim::UnitSnapshot before;
        sim::Tick tick_number = 0;
        if (observing) {
            before = sim.units();
            tick_number = sim.tick();
        }
        sim.step();
        // The sim's own word on it: a shot of the driven unit's, aimed at nothing, is the trigger.
        // No such shot means the sim declined it, and nothing is heard.
        if (!judged) {
            judged = true;
            bool launched = false;
            for (const sim::Sim::Shot& shot : sim.shots()) {
                launched = launched || (shot.shooter == frame.player && !shot.target.valid());
            }
            if (launched) {
                voices.push_back(game::shot_voice());
            } else {
                result.trigger_refused = true;
            }
        }
        for (const sim::Landing& landing : sim.landings()) {
            if (landing.shooter == frame.player && landing.lob) {
                result.touchdowns.push_back(AdvanceResult::Touchdown{sim.tick(), landing});
            }
        }
        if (!observing) {
            continue;
        }
        const std::vector<sim::BattleEvent> events = sim::events_between(before, sim, tick_number);
        if (game::unit_was_hit(events, frame.player)) {
            hit_flash.hit(sim.tick());
        }
        if (game::shooter_connected(events, frame.player, before)) {
            hit_mark.hit(sim.tick());
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
    return result;
}

} // namespace protekt::app
