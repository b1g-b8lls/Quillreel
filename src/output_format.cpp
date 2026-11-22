// SPDX-License-Identifier: GPL-3.0-or-later
#include "output_format.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace quillreel {
namespace {

const std::vector<OutputFormat>& table() {
    static const std::vector<OutputFormat> formats = {
        // Video containers
        {"mp4", "mp4", {"libx264"}, {"aac"}, true, false},
        {"m4v", "mp4", {"libx264"}, {"aac"}, true, false},
        {"mov", "mov", {"libx264"}, {"aac"}, true, false},
        {"mkv", "matroska", {"libx264"}, {"flac"}, true, false},
        {"webm", "webm", {"libvpx-vp9", "libsvtav1"}, {"libopus", "libvorbis"}, true, false},
        // Uncompressed YUV, exactly as decoded (large: about 3 MB per 1080p frame)
        {"y4m", "yuv4mpegpipe", {"rawvideo"}, {}, false, false},
        // Image sequence
        {"png", "image2", {"png"}, {}, false, true},
        // Audio only
        {"wav", "wav", {}, {"pcm_s16le"}, false, false},
        {"flac", "flac", {}, {"flac"}, false, false},
        {"m4a", "ipod", {}, {"aac"}, false, false},
        {"mka", "matroska", {}, {"flac"}, true, false},
        {"ogg", "ogg", {}, {"libvorbis", "libopus"}, false, false},
        {"opus", "opus", {}, {"libopus"}, false, false},
        {"mp3", "mp3", {}, {"libmp3lame"}, false, false},
    };
    return formats;
}

}  // namespace

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
