// SPDX-License-Identifier: GPL-3.0-or-later
// AudioSink (see player.h) on Qt Multimedia. The playback worker writes float
// PCM into a buffer; a QAudioSink in pull mode reads it on the GUI thread.
// Spec: specs/gui.md
#pragma once

#include <QObject>

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

#include "player.h"

class QAudioSink;

namespace quillreel::gui {

class QtAudioSink final : public QObject, public AudioSink {
    Q_OBJECT
public:
    // GUI thread. Returns null if there is no audio output device.
    static std::unique_ptr<QtAudioSink> open(int rate, int channels);
    ~QtAudioSink() override;

    // AudioSink, called from the worker thread (clear also from the GUI thread).
    bool configure(int rate, int channels) override;
    void write(const float* interleaved, int samples) override;
    void clear() override;
    void flush() override {}
    double buffered_seconds() const override;

    // GUI thread.
    void set_paused(bool paused);
    void set_volume(float volume);  // 0..1, linear

private:
    class Feed;
    friend class Feed;
    QtAudioSink();
    void restart();  // GUI thread: (re)create the QAudioSink for the current format
    qint64 read(char* data, qint64 max);

    using Clock = std::chrono::steady_clock;

    mutable std::mutex m_;
    std::vector<float> buf_;  // samples from head_ on are waiting to be played
    size_t head_ = 0;
    int rate_ = 48000, channels_ = 2;
    // What was handed to the device recently (time, seconds of real audio):
    // roughly what it still holds, which counts as buffered.
    mutable std::deque<std::pair<Clock::time_point, double>> handed_;
    double device_latency_ = 0.05;  // seconds the device buffer holds
    bool paused_ = false;
    Clock::time_point paused_at_{};

    // GUI thread only.
    Feed* feed_ = nullptr;
    QAudioSink* sink_ = nullptr;
    bool float_out_ = true;
    int device_channels_ = 0;
    float volume_ = 1.0f;
};

}  // namespace quillreel::gui
