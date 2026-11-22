// SPDX-License-Identifier: GPL-3.0-or-later
// The playback clock shared by the SDL and Qt players: a pausable wall clock
// that follows the audio position. Spec: specs/player.md ("Clock").
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>

namespace quillreel {

class SyncClock {
public:
    using Clock = std::chrono::steady_clock;

    double now() const { return paused_ ? pts_ : pts_ + seconds(Clock::now() - anchor_); }
    void set(double pts) {
        pts_ = pts;
        anchor_ = Clock::now();
    }
    void set_paused(bool paused) {
        set(now());
        paused_ = paused;
    }
    bool paused() const { return paused_; }

    // The audio position moves in device-sized steps (tens of ms), so jumping
    // to it would skip frames. Ease towards it with a ~250 ms time constant and
    // jump only for errors over 150 ms. Call regularly while audio is playing.
    void follow(double audio_pos) {
        const auto t = Clock::now();
        const double dt = seconds(t - last_follow_);
        last_follow_ = t;
        const double c = now();
        const double err = audio_pos - c;
        if (std::abs(err) > 0.15)
            set(c + err);
        else
            set(c + err * std::min(1.0, dt / 0.25));
    }

private:
    static double seconds(Clock::duration d) { return std::chrono::duration<double>(d).count(); }

    double pts_ = 0;
    Clock::time_point anchor_ = Clock::now();
    Clock::time_point last_follow_ = Clock::now();
    bool paused_ = false;
};

}  // namespace quillreel
