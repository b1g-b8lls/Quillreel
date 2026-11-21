// SPDX-License-Identifier: GPL-3.0-or-later
#include <unistd.h>

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

#include "convert.h"
#include "inspect.h"
#include "output_format.h"
#include "report.h"

namespace {

constexpr std::string_view kVersion = "0.2.0";

std::string usage_text() {
    return std::format(
        "Usage:\n"
        "  quillreel inspect [--json] [--verbose] FILE...\n"
        "  quillreel convert [options] INPUT OUTPUT\n"
        "  quillreel play                 (not implemented yet)\n"
        "\n"
        "inspect: print header information for Bink (.bik/.bk2) files.\n"
        "  --json             Print a JSON array, one object per file\n"
        "  --verbose          Show FFmpeg warnings\n"
        "\n"
        "convert: write a Bink file to another format, chosen by OUTPUT's extension:\n"
        "  {}\n"
        "  .png needs a frame number pattern, e.g. frames/%05d.png\n"
        "  --no-video         Leave out the video (audio extraction)\n"
        "  --no-audio         Leave out the audio\n"
        "  --audio-track N    Convert audio track N, numbered as in `inspect` (default 0)\n"
        "  --all-audio        Convert every audio track (mp4, mov, mkv, mka, webm)\n"
        "  -y, --overwrite    Replace OUTPUT if it exists\n"
        "  --verbose          Show FFmpeg warnings\n"
        "  --allow-non-bink   Accept any input FFmpeg can read (for testing)\n"
        "\n"
        "Exit codes: 0 ok, 1 usage error, 2 cannot open file, 3 not a Bink file,\n"
        "4 stream or format not supported by this FFmpeg build, 5 output error.\n"
        "inspect with several files returns the highest code.\n",
        quillreel::supported_extensions());
}

int usage_error(std::string_view msg) {
    std::cerr << "quillreel: " << msg << "\n\n" << usage_text();
    return 1;
}

int run_inspect(const std::vector<std::string>& args) {
    bool json = false;
    bool verbose = false;
    std::vector<std::string> files;
    bool options_done = false;
    for (const std::string& a : args) {
        if (!options_done && a == "--") {
            options_done = true;
        } else if (!options_done && a == "--json") {
            json = true;
        } else if (!options_done && a == "--verbose") {
            verbose = true;
        } else if (!options_done && a.starts_with("-") && a != "-") {
            return usage_error("unknown option: " + a);
        } else {
            files.push_back(a);
        }
    }
    if (files.empty()) return usage_error("inspect needs at least one file");

    av_log_set_level(verbose ? AV_LOG_WARNING : AV_LOG_ERROR);

    std::vector<quillreel::InspectResult> results;
    int exit_code = 0;
    for (const std::string& path : files) {
        auto r = quillreel::inspect_file(path);
        if (!r) exit_code = std::max(exit_code, static_cast<int>(r.error().kind));
        results.push_back({path, std::move(r)});
    }

    if (json) {
        quillreel::print_json(std::cout, results);
    } else {
        bool first = true;
        for (const auto& r : results) {
            if (r.result) {
                if (!first) std::cout << "\n";
                quillreel::print_text(std::cout, *r.result);
                first = false;
            } else {
                std::cerr << "quillreel: " << r.path << ": " << r.result.error().message << "\n";
            }
        }
    }
    return exit_code;
}

std::string format_seconds(double s) {
    auto t = static_cast<long long>(s);
    return std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60);
}

int run_convert(const std::vector<std::string>& args) {
    quillreel::ConvertOptions opts;
    bool verbose = false;
    std::vector<std::string> paths;
    bool options_done = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (options_done || a == "-" || !a.starts_with("-")) {
            paths.push_back(a);
        } else if (a == "--") {
            options_done = true;
        } else if (a == "--no-video") {
            opts.video = false;
        } else if (a == "--no-audio") {
            opts.audio = false;
        } else if (a == "--all-audio") {
            opts.all_audio = true;
        } else if (a == "-y" || a == "--overwrite") {
            opts.overwrite = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--allow-non-bink") {
            opts.allow_non_bink = true;
        } else if (a == "--audio-track") {
            if (i + 1 >= args.size()) return usage_error("--audio-track needs a number");
            const std::string& v = args[++i];
            int n = -1;
            auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            if (ec != std::errc() || end != v.data() + v.size() || n < 0)
                return usage_error("--audio-track needs a track number (0, 1, ...), got '" + v + "'");
            opts.audio_track = n;
        } else {
            return usage_error("unknown option: " + a);
        }
    }
    if (paths.size() != 2) return usage_error("convert needs exactly one INPUT and one OUTPUT");
    if (!opts.audio && (opts.audio_track || opts.all_audio))
        return usage_error("--no-audio cannot be combined with --audio-track or --all-audio");
    if (opts.audio_track && opts.all_audio) return usage_error("use either --audio-track or --all-audio, not both");
    if (!opts.video && !opts.audio) return usage_error("--no-video and --no-audio leave nothing to convert");
    opts.input = paths[0];
    opts.output = paths[1];

    av_log_set_level(verbose ? AV_LOG_WARNING : AV_LOG_ERROR);

    const bool show_progress = !verbose && isatty(STDERR_FILENO);
    quillreel::ProgressFn progress;
    if (show_progress) {
        progress = [](double done, double total) {
            if (total > 0)
                std::fprintf(stderr, "\rconverting: %s / %s (%3.0f%%)", format_seconds(done).c_str(),
                             format_seconds(total).c_str(), std::min(100.0, 100 * done / total));
            else
                std::fprintf(stderr, "\rconverting: %s", format_seconds(done).c_str());
        };
    }

    auto result = quillreel::convert_file(opts, progress);
    if (show_progress) std::fprintf(stderr, "\r\033[K");
    if (!result) {
        std::cerr << "quillreel: " << result.error().message << "\n";
        return static_cast<int>(result.error().kind);
    }

    const quillreel::ConvertStats& s = *result;
    std::string parts;
    if (s.video_frames >= 0) parts = std::format("{} video frames", s.video_frames);
    for (size_t i = 0; i < s.audio_tracks.size(); ++i) {
        double secs = s.audio_rates[i] ? static_cast<double>(s.audio_samples[i]) / s.audio_rates[i] : 0;
        parts += std::format("{}audio track {} ({:.2f} s)", parts.empty() ? "" : ", ", s.audio_tracks[i], secs);
    }
    std::cout << "wrote " << s.output << ": " << parts << "\n";
    if (!opts.all_audio && !opts.audio_track && s.input_audio_tracks > 1 && !s.audio_tracks.empty())
        std::cerr << "note: the input has " << s.input_audio_tracks
                  << " audio tracks; only track 0 was converted (see --audio-track, --all-audio)\n";
    if (s.decode_errors > 0)
        std::cerr << "warning: " << s.decode_errors << " damaged packet(s) skipped (use --verbose for details)\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error("no command given");

    const std::string cmd = args.front();
    args.erase(args.begin());

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::cout << usage_text();
        return 0;
    }
    if (cmd == "--version") {
        std::cout << "quillreel " << kVersion << "\n";
        return 0;
    }
    if (cmd == "inspect") return run_inspect(args);
    if (cmd == "convert") return run_convert(args);
    if (cmd == "play") {
        std::cerr << "quillreel: '" << cmd << "' is not implemented yet\n";
        return 1;
    }
    return usage_error("unknown command: " + cmd);
}
