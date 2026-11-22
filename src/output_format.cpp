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

// Codec choices per extension (first = default).
void add_codec_choices(std::vector<OutputFormat>& formats) {
    for (OutputFormat& f : formats) {
        const std::string& e = f.extension;
        if (e == "mp4" || e == "m4v") {
            f.video_codecs = {"h264", "h265", "av1"};
            f.audio_codecs = {"aac", "mp3", "opus", "flac"};
        } else if (e == "mov") {
            f.video_codecs = {"h264", "h265"};
            f.audio_codecs = {"aac"};
        } else if (e == "mkv") {
            f.video_codecs = {"h264", "h265", "av1", "vp9", "ffv1"};
            f.audio_codecs = {"flac", "aac", "opus", "vorbis", "mp3", "pcm"};
        } else if (e == "webm") {
            f.video_codecs = {"vp9", "av1"};
            f.audio_codecs = {"opus", "vorbis"};
        } else if (e == "mka") {
            f.audio_codecs = {"flac", "aac", "opus", "vorbis", "mp3", "pcm"};
        }
    }
}

}  // namespace

const std::vector<Codec>& codecs() {
    static const std::vector<Codec> list = {
        {"h264", "H.264", {"libx264"}, true, true, false},
        {"h265", "H.265 (HEVC)", {"libx265"}, true, true, false},
        {"av1", "AV1", {"libsvtav1"}, true, true, false},
        {"vp9", "VP9", {"libvpx-vp9"}, true, true, false},
        {"ffv1", "FFV1 (lossless)", {"ffv1"}, true, false, false},
        {"aac", "AAC", {"aac"}, false, false, true},
        {"opus", "Opus", {"libopus"}, false, false, true},
        {"vorbis", "Vorbis", {"libvorbis"}, false, false, true},
        {"mp3", "MP3", {"libmp3lame"}, false, false, true},
        {"flac", "FLAC (lossless)", {"flac"}, false, false, false},
        {"pcm", "PCM (uncompressed)", {"pcm_s16le"}, false, false, false},
    };
    return list;
}

const Codec* find_codec(const std::string& name) {
    for (const Codec& c : codecs())
        if (c.name == name) return &c;
    return nullptr;
}

bool codec_available(const Codec& c) {
    for (const std::string& n : c.encoders)
        if (avcodec_find_encoder_by_name(n.c_str())) return true;
    return false;
}

const std::vector<OutputFormat>& output_formats() {
    static const std::vector<OutputFormat> formats = [] {
        std::vector<OutputFormat> f = table();
        add_codec_choices(f);
        return f;
    }();
    return formats;
}

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
    for (const OutputFormat& f : output_formats())
        if (f.extension == ext) return f;
    return std::nullopt;
}

std::string supported_extensions() {
    std::string out;
    for (const OutputFormat& f : table()) out += (out.empty() ? "" : ", ") + f.extension;
    return out;
}

}  // namespace quillreel
