// SPDX-License-Identifier: GPL-3.0-or-later
#include "report.h"

#include <cmath>
#include <cstdio>
#include <format>

namespace quillreel {
namespace {

std::string format_rate(Rational r) {
    if (r.num <= 0 || r.den <= 0) return "unknown";
    if (r.num % r.den == 0) return std::to_string(r.num / r.den);
    return std::format("{:.3f} ({}/{})", static_cast<double>(r.num) / r.den, r.num, r.den);
}

std::string format_duration(double s) {
    if (s <= 0) return "unknown";
    auto total_ms = static_cast<long long>(std::llround(s * 1000));
    return std::format("{:02}:{:02}:{:02}.{:03} ({:.3f} s)", total_ms / 3600000,
                       total_ms / 60000 % 60, total_ms / 1000 % 60, total_ms % 1000, s);
}

std::string format_size(int64_t bytes) {
    if (bytes <= 0) return "unknown";
    return std::format("{:.1f} MiB ({} bytes)", bytes / (1024.0 * 1024.0), bytes);
}

std::string json_string(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
                out += std::format("\\u{:04x}", c);
            else
                out += static_cast<char>(c);
        }
    }
    return out + "\"";
}

void json_file(std::ostream& out, const FileInfo& f) {
    out << "    \"file_size\": " << f.file_size << ",\n";
    out << "    \"duration\": " << std::format("{:.6f}", f.duration_s) << ",\n";
    out << "    \"bit_rate\": " << f.bit_rate << ",\n";
    if (f.video) {
        const VideoInfo& v = *f.video;
        out << "    \"video\": {\n"
            << "      \"fourcc\": " << json_string(v.fourcc) << ",\n"
            << "      \"generation\": " << json_string(v.generation) << ",\n"
            << "      \"revision\": " << json_string(std::string(1, v.revision)) << ",\n"
            << "      \"width\": " << v.width << ",\n"
            << "      \"height\": " << v.height << ",\n"
            << "      \"frame_rate\": " << json_string(std::format("{}/{}", v.frame_rate.num, v.frame_rate.den)) << ",\n"
            << "      \"frame_count\": " << v.frame_count << ",\n"
            << "      \"keyframe_count\": " << (v.keyframe_count < 0 ? std::string("null") : std::to_string(v.keyframe_count)) << ",\n"
            << "      \"decoder\": " << (v.decoder.empty() ? std::string("null") : json_string(v.decoder)) << "\n"
            << "    },\n";
    } else {
        out << "    \"video\": null,\n";
    }
    out << "    \"audio\": [";
    for (size_t i = 0; i < f.audio.size(); ++i) {
        const AudioTrack& a = f.audio[i];
        out << (i ? ",\n" : "\n")
            << "      {\"stream_index\": " << a.stream_index << ", \"track_id\": " << a.track_id
            << ", \"codec\": " << json_string(a.codec) << ", \"sample_rate\": " << a.sample_rate
            << ", \"channels\": " << a.channels << ", \"decodable\": " << (a.decodable ? "true" : "false") << "}";
    }
    out << (f.audio.empty() ? "]\n" : "\n    ]\n");
}

}  // namespace

void print_text(std::ostream& out, const FileInfo& f) {
    out << f.path << "\n";
    if (f.video) {
        const VideoInfo& v = *f.video;
        out << "  Format:        " << v.generation << " (" << v.fourcc << ", revision " << v.revision << ")\n";
    }
    out << "  Size:          " << format_size(f.file_size) << "\n";
    out << "  Duration:      " << format_duration(f.duration_s) << "\n";
    out << "  Bit rate:      " << (f.bit_rate > 0 ? std::format("{} kb/s", f.bit_rate / 1000) : "unknown") << "\n";
    if (f.video) {
        const VideoInfo& v = *f.video;
        out << "  Video:         " << v.width << "x" << v.height << ", " << format_rate(v.frame_rate) << " fps, "
            << v.frame_count << " frames";
        if (v.keyframe_count >= 0) out << ", " << v.keyframe_count << " keyframes";
        out << "\n";
        out << "  Video decoder: " << (v.decoder.empty() ? "none in this FFmpeg build" : v.decoder) << "\n";
    } else {
        out << "  Video:         none\n";
    }
    out << "  Audio tracks:  " << f.audio.size() << "\n";
    for (size_t i = 0; i < f.audio.size(); ++i) {
        const AudioTrack& a = f.audio[i];
        out << std::format("    #{}  {}  {} Hz  {} ch  (stream {}, id {}){}\n", i, a.codec, a.sample_rate,
                           a.channels, a.stream_index, a.track_id, a.decodable ? "" : "  no decoder");
    }
}

void print_json(std::ostream& out, const std::vector<InspectResult>& results) {
    out << "[";
    for (size_t i = 0; i < results.size(); ++i) {
        const InspectResult& r = results[i];
        out << (i ? ",\n" : "\n") << "  {\n    \"path\": " << json_string(r.path) << ",\n";
        if (r.result) {
            json_file(out, *r.result);
        } else {
            out << "    \"error\": " << json_string(r.result.error().message) << ",\n"
                << "    \"exit_code\": " << static_cast<int>(r.result.error().kind) << "\n";
        }
        out << "  }";
    }
    out << "\n]\n";
}

}  // namespace quillreel
