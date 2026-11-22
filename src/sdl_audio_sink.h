// SPDX-License-Identifier: GPL-3.0-or-later
// AudioSink (see player.h) on an SDL3 audio stream, used by both players: the
// CLI's SDL window and the Qt GUI. (The GUI used Qt Multimedia before, which
// found no audio device under Wine and possibly on some Windows systems.)
// The header has no SDL types, so Qt code can include it.
// Spec: specs/player.md
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "player.h"

struct SDL_AudioStream;

namespace quillreel {

class SdlAudioSink final : public AudioSink {
public:
    // Opens the default playback device (starting SDL's audio subsystem for as
    // long as the sink lives). Null if there is none; *why then says why.
    static std::unique_ptr<SdlAudioSink> open(int rate, int channels, std::string* why = nullptr);
    ~SdlAudioSink() override;
    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    bool configure(int rate, int channels) override;
    void write(const float* interleaved, int samples) override;
    void clear() override;
    // When SDL resamples, it holds back a few samples until told the stream ended.
    void flush() override;
    double buffered_seconds() const override;

    void set_paused(bool paused);
    void set_gain(float gain);  // 0..1

private:
    SdlAudioSink(SDL_AudioStream* s, int rate, int channels);
    SDL_AudioStream* stream_;
    std::atomic<int> channels_;
    std::atomic<int> bytes_per_second_;
};

}  // namespace quillreel
