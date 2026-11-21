// SPDX-License-Identifier: GPL-3.0-or-later
// SDL3 window, audio output and controls for `quillreel play`. Spec: specs/player.md
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "player.h"

namespace quillreel {

struct UiOptions {
    bool loop = false;
    bool fullscreen = false;
    int volume = 100;     // percent, 0-100
    bool report = false;  // print playback statistics at exit (used by tests)
    // Synthetic key presses for tests: (seconds after start, SDL key name,
    // optionally prefixed "shift+"), e.g. {1.0, "Right"}.
    std::vector<std::pair<double, std::string>> test_keys;
};

// Runs until the file ends or the user quits. Returns the process exit code.
int play_file(const PlayOptions& opts, const UiOptions& ui);

}  // namespace quillreel
