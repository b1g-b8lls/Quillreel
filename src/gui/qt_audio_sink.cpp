// SPDX-License-Identifier: GPL-3.0-or-later
#include "qt_audio_sink.h"

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#include <QIODevice>
#include <QMediaDevices>

#include <algorithm>
#include <cstring>

namespace quillreel::gui {

// The QIODevice the QAudioSink pulls from.
class QtAudioSink::Feed final : public QIODevice {
public:
    explicit Feed(QtAudioSink* owner) : QIODevice(owner), owner_(owner) {}
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return 1 << 20; }  // always "ready": silence fills gaps

protected:
    qint64 readData(char* data, qint64 max) override { return owner_->read(data, max); }
    qint64 writeData(const char*, qint64) override { return -1; }

private:
    QtAudioSink* owner_;
};

QtAudioSink::QtAudioSink() = default;

QtAudioSink::~QtAudioSink() {
    if (sink_) sink_->stop();
}

std::unique_ptr<QtAudioSink> QtAudioSink::open(int rate, int channels) {
    if (QMediaDevices::defaultAudioOutput().isNull()) return nullptr;
    std::unique_ptr<QtAudioSink> s(new QtAudioSink);
    s->rate_ = rate;
    s->channels_ = channels;
    s->feed_ = new Feed(s.get());
    s->feed_->open(QIODevice::ReadOnly);
    s->restart();
    if (!s->sink_) return nullptr;
    return s;
}

void QtAudioSink::restart() {
    if (sink_) {
        sink_->stop();
        delete sink_;
        sink_ = nullptr;
    }
    int rate, channels;
    {
        std::lock_guard lk(m_);
        rate = rate_;
        channels = channels_;
        handed_.clear();
    }
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    QAudioFormat fmt;
    fmt.setSampleRate(rate);
    fmt.setChannelCount(channels);
    fmt.setSampleFormat(QAudioFormat::Float);
    float_out_ = dev.isFormatSupported(fmt);
    if (!float_out_) {
        fmt.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(fmt)) return;
    }
    sink_ = new QAudioSink(dev, fmt, this);
    // About 60 ms: short enough that the picture follows the sound closely.
    sink_->setBufferSize(fmt.bytesForDuration(60000));
    sink_->setVolume(volume_);
    device_channels_ = channels;
    sink_->start(feed_);
    const double latency = fmt.durationForBytes(static_cast<qint32>(sink_->bufferSize())) / 1e6;
    {
        std::lock_guard lk(m_);
        device_latency_ = latency > 0 ? latency : 0.05;
    }
    if (paused_) sink_->suspend();
}

bool QtAudioSink::configure(int rate, int channels) {
    {
        std::lock_guard lk(m_);
        rate_ = rate;
        channels_ = channels;
        buf_.clear();
        head_ = 0;
    }
    // The device must be recreated on the GUI thread; until then read() outputs
    // silence because the channel counts differ.
    QMetaObject::invokeMethod(this, [this] { restart(); }, Qt::QueuedConnection);
    return true;
}

void QtAudioSink::write(const float* interleaved, int samples) {
    std::lock_guard lk(m_);
    if (head_ > 0 && head_ * 2 > buf_.size()) {
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
    buf_.insert(buf_.end(), interleaved, interleaved + static_cast<size_t>(samples) * channels_);
}

void QtAudioSink::clear() {
    {
        std::lock_guard lk(m_);
        buf_.clear();
        head_ = 0;
        handed_.clear();
    }
    // Drop what the device still holds, too (GUI thread).
    QMetaObject::invokeMethod(
        this,
        [this] {
            if (!sink_) return;
            sink_->reset();
            sink_->start(feed_);
            if (paused_) sink_->suspend();
        },
        Qt::QueuedConnection);
}

double QtAudioSink::buffered_seconds() const {
    std::lock_guard lk(m_);
    const int ch = std::max(channels_, 1);
    double s = static_cast<double>(buf_.size() - head_) / ch / std::max(rate_, 1);
    const auto now = paused_ ? paused_at_ : Clock::now();
    const auto window = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(device_latency_));
    while (!handed_.empty() && now - handed_.front().first > window) handed_.pop_front();
    for (const auto& h : handed_) s += h.second;
    return s;
}

qint64 QtAudioSink::read(char* data, qint64 max) {
    std::lock_guard lk(m_);
    const int ch = channels_;
    const int bytes_per_sample = float_out_ ? 4 : 2;
    const qint64 frames = max / (bytes_per_sample * std::max(ch, 1));
    if (frames <= 0) return 0;
    const qint64 out_samples = frames * ch;
    size_t take = 0;
    if (ch == device_channels_) take = std::min<size_t>(buf_.size() - head_, static_cast<size_t>(out_samples));
    const float* src = buf_.data() + head_;
    if (float_out_) {
        std::memcpy(data, src, take * sizeof(float));
        std::memset(data + take * sizeof(float), 0, (static_cast<size_t>(out_samples) - take) * sizeof(float));
    } else {
        auto* out = reinterpret_cast<int16_t*>(data);
        for (size_t i = 0; i < take; ++i)
            out[i] = static_cast<int16_t>(std::clamp(src[i], -1.0f, 1.0f) * 32767.0f);
        std::fill(out + take, out + out_samples, int16_t{0});
    }
    head_ += take;
    if (take) handed_.emplace_back(Clock::now(), static_cast<double>(take) / ch / std::max(rate_, 1));
    return out_samples * bytes_per_sample;
}

void QtAudioSink::set_paused(bool paused) {
    {
        std::lock_guard lk(m_);
        if (paused == paused_) return;
        if (paused) {
            paused_at_ = Clock::now();
        } else {
            // Time stood still for what the device holds while paused.
            const auto gap = Clock::now() - paused_at_;
            for (auto& h : handed_) h.first += gap;
        }
        paused_ = paused;
    }
    if (!sink_) return;
    if (paused)
        sink_->suspend();
    else
        sink_->resume();
}

void QtAudioSink::set_volume(float volume) {
    volume_ = volume;
    if (sink_) sink_->setVolume(volume);
}

}  // namespace quillreel::gui
