#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "protekt/app/audio.hpp"
#include "protekt/game/scene.hpp"
#include "protekt/game/sounds.hpp"
#include "protekt/sim/sim.hpp"

namespace protekt::app {

/// Where finished stereo samples go. `AudioOut` is the real one; the seam exists so the pipeline
/// can be tested without a sound card.
class SampleSink {
public:
    SampleSink() = default;
    SampleSink(const SampleSink&) = delete;
    SampleSink& operator=(const SampleSink&) = delete;
    SampleSink(SampleSink&&) = delete;
    SampleSink& operator=(SampleSink&&) = delete;
    virtual ~SampleSink() = default;
    [[nodiscard]] virtual bool ok() const noexcept = 0;
    virtual void play(std::span<const float> interleaved_stereo, int sample_rate) noexcept = 0;
};

/// A `SampleSink` that is an `AudioOut`. Does not own it; `out` must outlive the sink.
class AudioOutSink final : public SampleSink {
public:
    explicit AudioOutSink(AudioOut& out) noexcept : out_(&out) {}
    [[nodiscard]] bool ok() const noexcept override { return out_->ok(); }
    void play(std::span<const float> interleaved_stereo, int sample_rate) noexcept override {
        out_->play(interleaved_stereo, sample_rate);
    }

private:
    AudioOut* out_;
};

/// What the pipeline needs to know about the frame it is stepping.
struct AudioFrame {
    game::Listener listener;
    /// The player pulled the trigger this frame. It does NOT mean a shot was made: the sound plays
    /// if and only if the sim launched one, which the pipeline reads from the sim's own `shots()`.
    bool trigger_pulled{false};
    /// Whether the player was alive before this frame's orders, which decides if the hit flash
    /// needs the per-tick copy of the units.
    bool player_alive_before{false};
    /// Whether the player is alive now: a dead player is listening from nowhere.
    bool player_alive_now{false};
    /// The tank being driven THIS frame, whose hits flash the screen. Read from the session each
    /// frame, never kept: a swap changes it.
    sim::UnitId player{};
};

/// What a call to `advance` learned that the caller must act on.
struct AdvanceResult {
    /// The trigger was pulled and the sim launched no shot for the driven unit on the step that
    /// applied it: the gun was reloading, or the turret not yet mounted.
    bool trigger_refused{false};
    /// Every landing of the PLAYER's own lob during the call, with the tick it came down on.
    struct Touchdown {
        sim::Tick tick{};
        sim::Landing landing{};
    };
    std::vector<Touchdown> touchdowns{};
};

/// Advances the sim and derives sound from it.
///
/// Sound is derived AFTER each step from the states either side of it, the way the replay log is:
/// the sim is not told, and the copy is only taken when someone is listening -- or when the player
/// is alive to be hit, for the flash. One mix covers every tick stepped in a call.
class AudioPipeline {
public:
    /// `sink` may be null (a headless or silent run); it is not owned.
    explicit AudioPipeline(SampleSink* sink) noexcept : sink_(sink) {}

    AdvanceResult advance(sim::Sim& sim, std::uint32_t steps, const AudioFrame& frame,
                          game::HitFlash& hit_flash, game::HitMark& hit_mark) const;

private:
    SampleSink* sink_;
};

} // namespace protekt::app
