// SPDX-License-Identifier: GPL-3.0-or-later
#include <signal.h>
#include <unistd.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
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

constexpr std::string_view kVersion = "0.7.0";

std::string usage_text() {
    return std::format(
        "Usage:\n"
        "  quillreel inspect [--json] [--verbose] FILE...\n"
        "  quillreel convert [options] INPUT OUTPUT\n"
        "  quillreel play [options] FILE\n"
        "  quillreel bink2 install DLL | status [DLL] | remove\n"
        "\n"
        "Bink 2 video (.bk2) is not supported out of the box: Quillreel includes no\n"
        "Bink 2 decoder. It is enabled once you provide RAD's bink2w64.dll from a game\n"
        "that uses Bink 2 (see `bink2` below). Until then, .bk2 files can be inspected\n"
        "and their audio played and converted, but not their video.\n"
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
        "  --quality LEVEL    Video quality: high, medium or small (default: near-lossless)\n"
        "  --crf N            Exact video quality instead (lower is better): H.264 0-51\n"
        "                     (default 18), VP9 0-63 (default 30)\n"
        "  --speed SPEED      Encoding speed: fast (bigger files), normal or slow (smaller)\n"
        "                     --quality, --crf and --speed: mp4, m4v, mov, mkv, webm\n"
        "  --width N, --height N\n"
        "                     Resize the video; with one of them the other keeps the\n"
        "                     aspect ratio (e.g. --height 720)\n"
        "  --audio-bitrate K  Audio bitrate in kb/s per track, for lossy audio (AAC,\n"
        "                     Opus, Vorbis, MP3; default 80 per channel)\n"
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
        "  Keys: space/k pause, j/l back/forward 10 s, left/right 5 s (shift: 30 s),\n"
        "        ,/. previous/next frame (pauses), 0-9 jump to 0-90 %, home/end start/end,\n"
        "        up/down volume, m mute, a/A next/previous audio track, r loop,\n"
        "        f or double-click fullscreen, q or Esc quit\n"
        "\n"
        "bink2: enable Bink 2 video by providing RAD's decoder, bink2w64.dll.\n"
        "  install DLL        Check a bink2w64.dll (64-bit, from any game that uses\n"
        "                     Bink 2) and copy it to {}.\n"
        "                     .bk2 video can then be played and converted.\n"
        "  status [DLL]       Show whether Bink 2 video is enabled, and with which DLL\n"
        "  remove             Delete the installed copy (disables Bink 2 video again)\n"
        "  QUILLREEL_BINK2_DLL=PATH uses that DLL instead, without installing it.\n"
        "\n"
        "Exit codes: 0 ok, 1 usage error, 2 cannot open file, 3 not a Bink file,\n"
        "4 stream or format not supported by this FFmpeg build, 5 output error,\n"
        "130 convert cancelled with Ctrl+C (the partial output is removed).\n"
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

// Ctrl+C (and SIGTERM/SIGHUP, or Ctrl+Break on Windows) during convert: ask the converter to stop, which
// deletes the partial output. A second signal ends the process at once.
std::atomic<bool> g_cancel{false};
static_assert(std::atomic<bool>::is_always_lock_free, "signal handler needs a lock-free flag");

extern "C" void on_cancel_signal(int sig) {
    if (g_cancel.exchange(true)) {
        signal(sig, SIG_DFL);
        raise(sig);
    }
}

void catch_cancel_signals() {
#ifdef _WIN32
    // The C runtime resets a handler to SIG_DFL when it runs, so a second
    // Ctrl+C ends the process as above.
    for (int sig : {SIGINT, SIGTERM, SIGBREAK}) signal(sig, on_cancel_signal);
#else
    struct sigaction sa {};
    sa.sa_handler = on_cancel_signal;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) sigaction(sig, &sa, nullptr);
#endif
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
        } else if (a == "--quality" || a == "--speed") {
            if (i + 1 >= args.size()) return usage_error(a + " needs a value");
            const std::string& v = args[++i];
            using Q = quillreel::ConvertOptions::Quality;
            using S = quillreel::ConvertOptions::Speed;
            if (a == "--quality" && v == "high") opts.quality = Q::High;
            else if (a == "--quality" && v == "medium") opts.quality = Q::Medium;
            else if (a == "--quality" && v == "small") opts.quality = Q::Small;
            else if (a == "--speed" && v == "fast") opts.speed = S::Fast;
            else if (a == "--speed" && v == "normal") opts.speed = S::Normal;
            else if (a == "--speed" && v == "slow") opts.speed = S::Slow;
            else
                return usage_error(a == "--quality" ? "--quality needs high, medium or small, got '" + v + "'"
                                                    : "--speed needs fast, normal or slow, got '" + v + "'");
        } else if (a == "--crf" || a == "--width" || a == "--height" || a == "--audio-bitrate") {
            if (i + 1 >= args.size()) return usage_error(a + " needs a number");
            const std::string& v = args[++i];
            int n = -1;
            auto [end, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
            const int min = a == "--crf" ? 0 : a == "--audio-bitrate" ? 8 : 16;
            const int max = a == "--crf" ? 63 : a == "--audio-bitrate" ? 1024 : 16384;
            if (ec != std::errc() || end != v.data() + v.size() || n < min || n > max)
                return usage_error(std::format("{} needs a number from {} to {}, got '{}'", a, min, max, v));
            if (a == "--crf") opts.crf = n;
            if (a == "--width") opts.width = n;
            if (a == "--height") opts.height = n;
            if (a == "--audio-bitrate") opts.audio_bitrate_kbps = n;
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
    if (opts.crf && opts.quality != quillreel::ConvertOptions::Quality::Default)
        return usage_error("use either --quality or --crf, not both");
    if (!opts.video && (opts.crf || opts.width || opts.height ||
                        opts.quality != quillreel::ConvertOptions::Quality::Default ||
                        opts.speed != quillreel::ConvertOptions::Speed::Normal))
        return usage_error("--no-video cannot be combined with video options (--quality, --crf, --speed, --width, --height)");
    if (!opts.audio && opts.audio_bitrate_kbps) return usage_error("--no-audio cannot be combined with --audio-bitrate");
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

    opts.cancel = &g_cancel;
    catch_cancel_signals();
    auto result = quillreel::convert_file(opts, progress);
    if (show_progress) std::fprintf(stderr, "\r\033[K");
    if (!result && g_cancel) {
        std::cerr << "quillreel: cancelled; the partial output was removed\n";
        return 130;  // 128 + SIGINT, as shells report Ctrl+C
    }
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
        std::cout << "Bink 2 video enabled: .bk2 video can now be played and converted\n";
        report(*r);
        return 0;
    }
    if (sub == "status") {
        if (args.size() > 2) return usage_error("bink2 status takes at most one DLL path");
        std::optional<std::string> path = args.size() == 2 ? std::optional(args[1]) : b2::dll_path();
        if (!path) {
            std::cout << "Bink 2 video: not enabled (no bink2w64.dll provided)\n"
                      << ".bk2 files can be inspected and their audio played and converted.\n"
                      << b2::missing_message() << "\n";
            return 0;
        }
        auto r = b2::check(*path);
        if (!r) {
            std::cerr << "quillreel: " << r.error().message << "\n";
            return static_cast<int>(r.error().kind);
        }
        std::cout << "Bink 2 video: enabled (the DLL loads)";
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
        std::cout << (*r ? "removed " + b2::installed_path() + "; Bink 2 video is disabled"
                         : std::string("no installed Bink 2 decoder to remove"))
                  << "\n";
        return 0;
    }
    return usage_error(sub.empty() ? "bink2 needs a subcommand: install, status or remove"
                                   : "unknown bink2 subcommand: " + sub);
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    // argv and file names are UTF-8 (data/windows/quillreel.manifest); make the
    // console show UTF-8 output too.
    SetConsoleOutputCP(CP_UTF8);
#endif
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
