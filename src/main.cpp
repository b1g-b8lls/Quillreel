// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

extern "C" {
#include <libavutil/log.h>
}

#include "inspect.h"
#include "report.h"

namespace {

constexpr std::string_view kVersion = "0.1.0";

constexpr std::string_view kUsage =
    "Usage: quillreel <command> [options] FILE...\n"
    "\n"
    "Commands:\n"
    "  inspect    Print header information for Bink (.bik/.bk2) files\n"
    "  play       (not implemented yet)\n"
    "  convert    (not implemented yet)\n"
    "\n"
    "Inspect options:\n"
    "  --json     Print a JSON array, one object per file\n"
    "  --verbose  Show FFmpeg warnings\n"
    "\n"
    "Exit codes: 0 ok, 1 usage error, 2 cannot open file, 3 not a Bink file.\n"
    "With several files, the highest exit code wins.\n";

int usage_error(std::string_view msg) {
    std::cerr << "quillreel: " << msg << "\n\n" << kUsage;
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage_error("no command given");

    const std::string cmd = args.front();
    args.erase(args.begin());

    if (cmd == "-h" || cmd == "--help" || cmd == "help") {
        std::cout << kUsage;
        return 0;
    }
    if (cmd == "--version") {
        std::cout << "quillreel " << kVersion << "\n";
        return 0;
    }
    if (cmd == "inspect") return run_inspect(args);
    if (cmd == "play" || cmd == "convert") {
        std::cerr << "quillreel: '" << cmd << "' is not implemented yet\n";
        return 1;
    }
    return usage_error("unknown command: " + cmd);
}
