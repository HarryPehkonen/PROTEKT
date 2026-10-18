#include "protekt/app/audio.hpp"

#include <cstddef>
#include <utility>

#ifdef PROTEKT_HAVE_SDL_AUDIO
#include <SDL3/SDL.h>
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
        SDL_DestroyAudioStream(impl_->stream); // closes the device it opened
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
