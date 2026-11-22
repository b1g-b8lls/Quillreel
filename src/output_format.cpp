// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_format.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace quillreel {
namespace {

const std::vector<OutputFormat>& table() {
    static const std::vector<OutputFormat> formats = {
        // Video containers
        {"mp4", "mp4", {"libx264"}, {"aac"}, true, false, "MP4 (H.264 video, AAC audio)", true, true},
        {"m4v", "mp4", {"libx264"}, {"aac"}, true, false, "M4V (H.264 video, AAC audio)", true, true},
        {"mov", "mov", {"libx264"}, {"aac"}, true, false, "QuickTime MOV (H.264 video, AAC audio)", true, true},
        {"mkv", "matroska", {"libx264"}, {"flac"}, true, false, "Matroska MKV (H.264 video, FLAC audio)", true, false},
        {"webm", "webm", {"libvpx-vp9", "libsvtav1"}, {"libopus", "libvorbis"}, true, false, "WebM (VP9 video, Opus audio)", true, true},
        // Uncompressed YUV, exactly as decoded (large: about 3 MB per 1080p frame)
        {"y4m", "yuv4mpegpipe", {"rawvideo"}, {}, false, false, "Y4M (uncompressed video, exactly as decoded; very large)"},
        // Image sequence
        {"png", "image2", {"png"}, {}, false, true, "PNG image sequence (one file per frame)"},
        // Audio only
        {"wav", "wav", {}, {"pcm_s16le"}, false, false, "WAV (audio only, uncompressed)"},
        {"flac", "flac", {}, {"flac"}, false, false, "FLAC (audio only, lossless)"},
        {"m4a", "ipod", {}, {"aac"}, false, false, "M4A (audio only, AAC)", false, true},
        {"mka", "matroska", {}, {"flac"}, true, false, "Matroska MKA (audio only, FLAC; can hold all tracks)"},
        {"ogg", "ogg", {}, {"libvorbis", "libopus"}, false, false, "Ogg (audio only, Vorbis)", false, true},
        {"opus", "opus", {}, {"libopus"}, false, false, "Opus (audio only)", false, true},
        {"mp3", "mp3", {}, {"libmp3lame"}, false, false, "MP3 (audio only)", false, true},
    };
    return formats;
}

}  // namespace

const std::vector<OutputFormat>& output_formats() { return table(); }

bool output_format_available(const OutputFormat& f) {
    auto any = [](const std::vector<std::string>& names) {
        for (const std::string& n : names)
            if (avcodec_find_encoder_by_name(n.c_str())) return true;
        return false;
    };
    return (f.video_encoders.empty() || any(f.video_encoders)) && (f.audio_encoders.empty() || any(f.audio_encoders));
}

std::optional<OutputFormat> output_format_for(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    if (ext.size() < 2) return std::nullopt;
    ext.erase(0, 1);
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return std::tolower(c); });
    for (const OutputFormat& f : table())
        if (f.extension == ext) return f;
    return std::nullopt;
}

std::string supported_extensions() {
    std::string out;
    for (const OutputFormat& f : table()) out += (out.empty() ? "" : ", ") + f.extension;
    return out;
}

}  // namespace quillreel
