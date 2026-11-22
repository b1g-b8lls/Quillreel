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

#include "bink2_dll.h"
#include "convert.h"
#include "inspect.h"
#include "output_format.h"
#include "play_sdl.h"
#include "report.h"

namespace {

constexpr std::string_view kVersion = "0.3.0";

std::string usage_text() {
    return std::format(
        "Usage:\n"
        "  quillreel inspect [--json] [--verbose] FILE...\n"
        "  quillreel convert [options] INPUT OUTPUT\n"
        "  quillreel play [options] FILE\n"
        "  quillreel bink2 install DLL | status [DLL] | remove\n"
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
        "play: show a Bink file in a window with sound.\n"
        "  --audio-track N    Start with audio track N (default 0)\n"
        "  --start SECONDS    Start at this position\n"
        "  --volume PERCENT   Initial volume, 0-100 (default 100)\n"
        "  --loop             Start again at the end\n"
        "  --fullscreen       Open fullscreen\n"
        "  --no-video / --no-audio, --verbose, --allow-non-bink: as for convert\n"
        "  Keys: space/k pause, left/right seek 5 s (shift: 30 s), home restart,\n"
        "        up/down volume, m mute, a/A next/previous audio track, l loop,\n"
        "        f or double-click fullscreen, q or Esc quit\n"
        "\n"
        "bink2: Bink 2 video (.bk2) is decoded by RAD's bink2w64.dll, which Quillreel\n"
        "does not include. Without it, .bk2 files play and convert audio only.\n"
        "  install DLL        Check a bink2w64.dll (64-bit, from any game that uses\n"
        "                     Bink 2) and copy it to {}\n"
        "  status [DLL]       Show which DLL is used and whether it loads\n"
        "  remove             Delete the installed copy\n"
        "  QUILLREEL_BINK2_DLL=PATH overrides the installed copy.\n"
        "\n"
        "Exit codes: 0 ok, 1 usage error, 2 cannot open file, 3 not a Bink file,\n"
        "4 stream or format not supported by this FFmpeg build, 5 output error.\n"
        "inspect with several files returns the highest code.\n",
        quillreel::supported_extensions(), quillreel::bink2::installed_path());
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

template <class T>
bool parse_number(const std::string& s, T& out) {
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return ec == std::errc() && end == s.data() + s.size();
}

int run_play(const std::vector<std::string>& args) {
    quillreel::PlayOptions opts;
    quillreel::UiOptions ui;
    bool verbose = false;
    std::vector<std::string> paths;
    bool options_done = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto value = [&]() -> const std::string* { return i + 1 < args.size() ? &args[++i] : nullptr; };
        if (options_done || a == "-" || !a.starts_with("-")) {
            paths.push_back(a);
        } else if (a == "--") {
            options_done = true;
        } else if (a == "--no-video") {
            opts.video = false;
        } else if (a == "--no-audio") {
            opts.audio = false;
        } else if (a == "--loop") {
            ui.loop = true;
        } else if (a == "--fullscreen") {
            ui.fullscreen = true;
        } else if (a == "--verbose") {
            verbose = true;
        } else if (a == "--allow-non-bink") {
            opts.allow_non_bink = true;
        } else if (a == "--report") {
            ui.report = true;
        } else if (a == "--test-keys") {  // hidden, for tests: "1.0:Right,2.5:shift+A"
            const std::string* v = value();
            if (!v) return usage_error("--test-keys needs a list like 1.0:Right,2.5:A");
            std::string_view rest = *v;
            while (!rest.empty()) {
                std::string_view item = rest.substr(0, rest.find(','));
                rest.remove_prefix(std::min(rest.size(), item.size() + 1));
                size_t colon = item.find(':');
                double at = -1;
                if (colon == std::string_view::npos || !parse_number(std::string(item.substr(0, colon)), at) || at < 0)
                    return usage_error("bad --test-keys entry: " + std::string(item));
                ui.test_keys.emplace_back(at, std::string(item.substr(colon + 1)));
            }
        } else if (a == "--audio-track") {
            const std::string* v = value();
            int n = -1;
            if (!v || !parse_number(*v, n) || n < 0) return usage_error("--audio-track needs a track number (0, 1, ...)");
            opts.audio_track = n;
        } else if (a == "--start") {
            const std::string* v = value();
            double s = -1;
            if (!v || !parse_number(*v, s) || !(s >= 0)) return usage_error("--start needs a time in seconds, e.g. 62.5");
            opts.start = s;
        } else if (a == "--volume") {
            const std::string* v = value();
            int n = -1;
            if (!v || !parse_number(*v, n) || n < 0 || n > 100) return usage_error("--volume needs a percentage, 0-100");
            ui.volume = n;
        } else {
            return usage_error("unknown option: " + a);
        }
    }
    if (paths.size() != 1) return usage_error("play needs exactly one FILE");
    if (!opts.audio && opts.audio_track) return usage_error("--no-audio cannot be combined with --audio-track");
    if (!opts.video && !opts.audio) return usage_error("--no-video and --no-audio leave nothing to play");
    opts.input = paths[0];

    av_log_set_level(verbose ? AV_LOG_WARNING : AV_LOG_ERROR);
    return quillreel::play_file(opts, ui);
}

int run_bink2(const std::vector<std::string>& args) {
    namespace b2 = quillreel::bink2;
    const std::string sub = args.empty() ? "" : args[0];
    auto report = [](const b2::DllInfo& info) {
        std::cout << "  path:    " << info.path << "\n"
                  << "  version: " << (info.version.empty() ? "unknown" : info.version) << "\n";
    };
    if (sub == "install") {
        if (args.size() != 2) return usage_error("bink2 install needs the path of a bink2w64.dll");
        auto r = b2::install(args[1]);
        if (!r) {
            std::cerr << "quillreel: " << r.error().message << "\n";
            return static_cast<int>(r.error().kind);
        }
        std::cout << "installed RAD's Bink 2 decoder; .bk2 video can now be played and converted\n";
        report(*r);
        return 0;
    }
    if (sub == "status") {
        if (args.size() > 2) return usage_error("bink2 status takes at most one DLL path");
        std::optional<std::string> path = args.size() == 2 ? std::optional(args[1]) : b2::dll_path();
        if (!path) {
            std::cout << "no Bink 2 decoder installed: .bk2 files play and convert audio only\n"
                      << b2::missing_message() << "\n";
            return 0;
        }
        auto r = b2::check(*path);
        if (!r) {
            std::cerr << "quillreel: " << r.error().message << "\n";
            return static_cast<int>(r.error().kind);
        }
        std::cout << "Bink 2 decoder loads and is ready";
        if (args.size() < 2 && std::getenv("QUILLREEL_BINK2_DLL")) std::cout << " (from QUILLREEL_BINK2_DLL)";
        std::cout << "\n";
        report(*r);
        return 0;
    }
    if (sub == "remove") {
        if (args.size() != 1) return usage_error("bink2 remove takes no arguments");
        auto r = b2::uninstall();
        if (!r) {
            std::cerr << "quillreel: " << r.error().message << "\n";
            return static_cast<int>(r.error().kind);
        }
        std::cout << (*r ? "removed " + b2::installed_path() : std::string("no installed Bink 2 decoder to remove")) << "\n";
        return 0;
    }
    return usage_error(sub.empty() ? "bink2 needs a subcommand: install, status or remove"
                                   : "unknown bink2 subcommand: " + sub);
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
    if (cmd == "play") return run_play(args);
    if (cmd == "bink2") return run_bink2(args);
    return usage_error("unknown command: " + cmd);
}
