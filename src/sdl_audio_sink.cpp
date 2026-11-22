// SPDX-License-Identifier: GPL-3.0-or-later
#include "sdl_audio_sink.h"

#include <SDL3/SDL.h>

namespace quillreel {

std::unique_ptr<SdlAudioSink> SdlAudioSink::open(int rate, int channels, std::string* why) {
    // Names the stream in the desktop's sound settings (PipeWire, PulseAudio).
    SDL_SetHint(SDL_HINT_APP_NAME, "Quillreel");
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (why) *why = SDL_GetError();
        return nullptr;
    }
    SDL_AudioSpec spec{SDL_AUDIO_F32, channels, rate};
    SDL_AudioStream* s = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!s) {
        if (why) *why = SDL_GetError();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        return nullptr;
    }
    return std::unique_ptr<SdlAudioSink>(new SdlAudioSink(s, rate, channels));
}

SdlAudioSink::SdlAudioSink(SDL_AudioStream* s, int rate, int channels)
    : stream_(s), channels_(channels), bytes_per_second_(rate * channels * static_cast<int>(sizeof(float))) {}

SdlAudioSink::~SdlAudioSink() {
    SDL_DestroyAudioStream(stream_);
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
}

bool SdlAudioSink::configure(int rate, int channels) {
    SDL_AudioSpec spec{SDL_AUDIO_F32, channels, rate};
    if (!SDL_SetAudioStreamFormat(stream_, &spec, nullptr)) return false;
    channels_ = channels;
    bytes_per_second_ = rate * channels * static_cast<int>(sizeof(float));
    return true;
}

void SdlAudioSink::write(const float* data, int samples) {
    SDL_PutAudioStreamData(stream_, data, samples * channels_ * static_cast<int>(sizeof(float)));
}

void SdlAudioSink::clear() { SDL_ClearAudioStream(stream_); }

void SdlAudioSink::flush() { SDL_FlushAudioStream(stream_); }

double SdlAudioSink::buffered_seconds() const {
    const int queued = SDL_GetAudioStreamQueued(stream_);
    return queued > 0 ? static_cast<double>(queued) / bytes_per_second_ : 0;
}

void SdlAudioSink::set_paused(bool paused) {
    if (paused)
        SDL_PauseAudioStreamDevice(stream_);
    else
        SDL_ResumeAudioStreamDevice(stream_);
}

void SdlAudioSink::set_gain(float gain) { SDL_SetAudioStreamGain(stream_, gain); }

}  // namespace quillreel
