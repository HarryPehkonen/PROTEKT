#include "protekt/app/audio.hpp"

#include <cstddef>
#include <utility>

#ifdef PROTEKT_HAVE_SDL_AUDIO
#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#endif

namespace protekt::app {

struct AudioOut::Impl {
#ifdef PROTEKT_HAVE_SDL_AUDIO
    SDL_AudioStream* stream{nullptr};
    int rate{0};
#endif
    bool healthy{true};
};

AudioOut::AudioOut(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

#ifdef PROTEKT_HAVE_SDL_AUDIO

namespace {

/// How long the device close may take before the game leaves without it.
///
/// A healthy close measures a couple of milliseconds, so anything near this budget is a block
/// rather than slow work. The number is the player's patience at a quit, not a device parameter.
constexpr int kCloseBudgetMs = 2000;

/// Closes the device stream, or leaves the process instead of freezing the window on it.
///
/// SDL's close takes the mutex guarding its logical audio devices, and that mutex can be left held
/// by SDL's own threads: on the report of 2026-11-03 (SDL 3.4.18 on PipeWire) a
/// default-device-change event put an SDL thread inside the driver's device open while holding it,
/// with PipeWire's data loop waiting for the same mutex, so a quit blocked here for ever. The game
/// cannot repair the driver, so it bounds the wait: the close runs on a helper thread and the
/// process leaves if it does not return.
void close_stream_bounded(SDL_AudioStream* stream) {
    std::atomic<bool> closed{false};
    std::thread closer([stream, &closed] {
        SDL_DestroyAudioStream(stream);
        closed.store(true, std::memory_order_release);
    });
    const auto deadline
        = std::chrono::steady_clock::now() + std::chrono::milliseconds(kCloseBudgetMs);
    while (!closed.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!closed.load(std::memory_order_acquire)) {
        // `std::_Exit`, not `std::exit`: it skips the joinable closer's destructor (which would
        // terminate the process anyway) and every later teardown step, all of which take the same
        // wedged mutex. The status is NOT zero: the shutdown did not finish, and a zero here would
        // let a truncated close read as a clean exit -- which is precisely how it would read inside
        // a test run. The cout flush is for a redirected stdout, which is block-buffered: the quit
        // line the player just saw must not be lost with it.
        std::cout.flush();
        std::cerr << "protekt-game: the audio device did not close within " << kCloseBudgetMs
                  << " ms; exiting without it\n";
        std::_Exit(1);
    }
    closer.join();
}

} // namespace

std::unique_ptr<AudioOut> AudioOut::open(std::string& error) {
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        error = std::string("no audio: ") + SDL_GetError();
        return nullptr;
    }
    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = 2;
    spec.freq = 48000;
    // No callback: SDL pulls from the stream's own queue, and the game thread is the only mixer.
    SDL_AudioStream* stream
        = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (stream == nullptr) {
        error = std::string("no audio device: ") + SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        error = std::string("audio device would not start: ") + SDL_GetError();
        SDL_DestroyAudioStream(stream);
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->stream = stream;
    impl->rate = spec.freq;
    return std::unique_ptr<AudioOut>(new AudioOut(std::move(impl)));
}

bool AudioOut::ok() const noexcept { return impl_ != nullptr && impl_->healthy; }

void AudioOut::play(std::span<const float> interleaved_stereo, int sample_rate) noexcept {
    if (!ok() || interleaved_stereo.empty()) {
        return;
    }
    // The device was opened at one rate; a buffer made at another would play at the wrong pitch.
    // Refusing it is honest, and the one caller passes the rate it was opened with.
    if (sample_rate != impl_->rate) {
        return;
    }
    const int queued = SDL_GetAudioStreamQueued(impl_->stream);
    const int cap = static_cast<int>(kMaxQueuedSeconds * static_cast<float>(impl_->rate)) * 2
                    * static_cast<int>(sizeof(float));
    if (queued < 0) {
        impl_->healthy = false;
        return;
    }
    if (queued > cap) {
        return;
    }
    const int bytes = static_cast<int>(interleaved_stereo.size() * sizeof(float));
    if (!SDL_PutAudioStreamData(impl_->stream, interleaved_stereo.data(), bytes)) {
        impl_->healthy = false;
    }
}

float AudioOut::queued_seconds() const noexcept {
    if (!ok()) {
        return 0.0F;
    }
    const int queued = SDL_GetAudioStreamQueued(impl_->stream);
    return queued < 0 ? 0.0F
                      : static_cast<float>(queued)
                            / static_cast<float>(impl_->rate * 2 * static_cast<int>(sizeof(float)));
}

AudioOut::~AudioOut() {
    if (impl_ != nullptr && impl_->stream != nullptr) {
        close_stream_bounded(
            impl_->stream); // closes the device it opened, or leaves rather than hangs
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
}

#else

std::unique_ptr<AudioOut> AudioOut::open(std::string& error) {
    error = "no audio: this build has no SDL3";
    return nullptr;
}

bool AudioOut::ok() const noexcept { return false; }

float AudioOut::queued_seconds() const noexcept { return 0.0F; }

void AudioOut::play(std::span<const float> /*interleaved_stereo*/, int /*sample_rate*/) noexcept {}

AudioOut::~AudioOut() = default;

#endif

} // namespace protekt::app
