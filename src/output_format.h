// SPDX-License-Identifier: GPL-3.0-or-later
// Maps an output file name to a container and encoder choices. Spec: specs/converter.md
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace quillreel {

struct OutputFormat {
    std::string extension;                    // lowercase, without the dot
    std::string muxer;                        // libavformat short name
    std::vector<std::string> video_encoders;  // in order of preference; empty = no video
    std::vector<std::string> audio_encoders;  // in order of preference; empty = no audio
    bool multi_audio = false;                 // can hold more than one audio track
    bool image_sequence = false;              // path must contain a frame number pattern
    std::string description;                  // for menus, e.g. "MP4 (H.264 video, AAC audio)"
    bool video_quality = false;               // takes --quality/--crf/--speed (H.264, VP9, AV1)
    bool audio_bitrate = false;               // lossy audio: takes --audio-bitrate
};

// Every supported format, in menu order.
const std::vector<OutputFormat>& output_formats();

// Whether this FFmpeg build has an encoder for each kind of stream the format holds.
bool output_format_available(const OutputFormat& f);

std::optional<OutputFormat> output_format_for(const std::string& path);

// Comma-separated list of supported extensions, for error messages and --help.
std::string supported_extensions();

}  // namespace quillreel
