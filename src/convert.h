// SPDX-License-Identifier: GPL-3.0-or-later
// Bink to common formats via FFmpeg (decode -> filter -> encode -> mux). Spec: specs/converter.md
#pragma once

#include <atomic>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "errors.h"

namespace quillreel {

struct ConvertOptions {
    std::string input;
    std::string output;
    bool video = true;               // false: --no-video
    bool audio = true;               // false: --no-audio
    std::optional<int> audio_track;  // position in the inspect listing (0-based)
    bool all_audio = false;
    bool overwrite = false;
    bool allow_non_bink = false;  // developer/testing: accept any FFmpeg-readable input

    // Quality (spec: specs/converter.md "Quality options"). Defaults keep the
    // built-in settings.
    enum class Quality { Default, High, Medium, Small };
    enum class Speed { Normal, Fast, Slow };
    Quality quality = Quality::Default;  // video quality level (CRF per encoder)
    std::optional<int> crf;              // exact CRF, overrides quality
    Speed speed = Speed::Normal;         // encoder speed/compression trade-off
    int width = 0, height = 0;           // output size; 0 = keep (one side alone keeps the aspect ratio)
    int audio_bitrate_kbps = 0;          // per audio track, lossy encoders only; 0 = 80 kb/s per channel

    // Set (from another thread) to stop; the conversion then fails with "cancelled"
    // and the partial output is deleted.
    const std::atomic<bool>* cancel = nullptr;
};

struct ConvertStats {
    std::string output;
    int64_t video_frames = -1;          // -1 when no video was written
    std::vector<int> audio_tracks;      // input track positions that were written
    std::vector<int64_t> audio_samples; // samples written per track, same order
    std::vector<int> audio_rates;       // output sample rate per track
    int input_audio_tracks = 0;
    int64_t decode_errors = 0;
};

// Called with seconds converted so far and the expected total (0 if unknown).
using ProgressFn = std::function<void(double done_s, double total_s)>;

std::expected<ConvertStats, Error> convert_file(const ConvertOptions& opts, const ProgressFn& progress = {});

}  // namespace quillreel
