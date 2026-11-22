// SPDX-License-Identifier: GPL-3.0-or-later
// Bink file inspection on top of FFmpeg's libavformat. Spec: specs/inspector.md
#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "errors.h"

namespace quillreel {

struct Rational {
    int num = 0;
    int den = 1;
};

struct VideoInfo {
    std::string fourcc;      // codec tag as text, e.g. "KB2g" or "BIKi"
    std::string generation;  // "Bink 1", "Bink 2" or "unknown"
    char revision = 0;       // last character of the fourcc
    int width = 0;
    int height = 0;
    Rational frame_rate;
    int64_t frame_count = 0;
    int64_t keyframe_count = -1;  // -1 when the demuxer gave no index
    std::string decoder;          // FFmpeg decoder name, "bink2w64.dll" for Bink 2 with the DLL, empty if none
};

struct AudioTrack {
    int stream_index = 0;
    int track_id = 0;  // AVStream::id as set by the demuxer
    std::string codec;
    int sample_rate = 0;
    int channels = 0;
    bool decodable = false;
};

struct FileInfo {
    std::string path;
    int64_t file_size = 0;
    double duration_s = 0;  // 0 when unknown
    int64_t bit_rate = 0;   // bits per second, 0 when unknown
    std::optional<VideoInfo> video;
    std::vector<AudioTrack> audio;
};

// Errors: ErrorKind::Open or ErrorKind::NotBink.
std::expected<FileInfo, Error> inspect_file(const std::string& path);

}  // namespace quillreel
