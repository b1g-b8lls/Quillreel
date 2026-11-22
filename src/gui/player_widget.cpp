// SPDX-License-Identifier: GPL-3.0-or-later
#include "player_widget.h"

#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <format>

#include "qt_audio_sink.h"
#include "video_view.h"

namespace quillreel::gui {

// A slider that jumps to where it is clicked (the default steps by a page).
class SeekSlider final : public QSlider {
public:
    using QSlider::QSlider;

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) {
            QStyleOptionSlider opt;
            initStyleOption(&opt);
            QRect handle = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderHandle, this);
            if (!handle.contains(e->position().toPoint())) {
                QRect groove = style()->subControlRect(QStyle::CC_Slider, &opt, QStyle::SC_SliderGroove, this);
                int span = groove.width() - handle.width();
                int x = static_cast<int>(e->position().x()) - groove.x() - handle.width() / 2;
                setValue(QStyle::sliderValueFromPosition(minimum(), maximum(), x, std::max(span, 1)));
            }
        }
        QSlider::mousePressEvent(e);  // starts dragging the handle, now under the cursor
    }
};

namespace {

QString format_time(double s) {
    auto t = static_cast<long long>(std::max(0.0, s));
    return QString::fromStdString(std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60));
}

QToolButton* tool_button(QWidget* parent, QStyle::StandardPixmap icon, const QString& tip) {
    auto* b = new QToolButton(parent);
    b->setIcon(parent->style()->standardIcon(icon));
    b->setToolTip(tip);
    b->setAutoRaise(true);
    b->setFocusPolicy(Qt::NoFocus);
    return b;
}

}  // namespace

PlayerWidget::PlayerWidget(QWidget* parent) : QWidget(parent) {
    setFocusPolicy(Qt::StrongFocus);

    view_ = new VideoView(this);
    note_ = new QLabel(this);
    note_->setAlignment(Qt::AlignCenter);
    note_->setWordWrap(true);
    note_->setMargin(24);
    note_->setAutoFillBackground(true);
    QPalette pal = note_->palette();
    pal.setColor(QPalette::Window, Qt::black);
    pal.setColor(QPalette::WindowText, QColor(220, 220, 220));
    note_->setPalette(pal);
    stack_ = new QStackedWidget(this);
    stack_->addWidget(view_);
    stack_->addWidget(note_);
    show_note(tr("Open a Bink file to play it.\nDouble-click a file in the list, or use File > Open."));

    play_ = tool_button(this, QStyle::SP_MediaPlay, tr("Play / pause (Space)"));
    restart_ = tool_button(this, QStyle::SP_MediaSkipBackward, tr("Restart (Home)"));
    position_ = new SeekSlider(Qt::Horizontal, this);
    position_->setFocusPolicy(Qt::NoFocus);
    position_->setToolTip(tr("Position (Left/Right: 5 s, with Shift: 30 s)"));
    time_ = new QLabel(format_time(0) + " / " + format_time(0), this);
    time_->setMinimumWidth(time_->fontMetrics().horizontalAdvance(" 0:00:00 / 0:00:00 "));
    tracks_ = new QComboBox(this);
    tracks_->setFocusPolicy(Qt::NoFocus);
    tracks_->setToolTip(tr("Audio track (A / Shift+A)"));
    tracks_->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    mute_ = tool_button(this, QStyle::SP_MediaVolume, tr("Mute (M)"));
    volume_ = new QSlider(Qt::Horizontal, this);
    volume_->setRange(0, 100);
    volume_->setValue(100);
    volume_->setFixedWidth(90);
    volume_->setFocusPolicy(Qt::NoFocus);
    volume_->setToolTip(tr("Volume (Up/Down when the video has focus)"));
    loop_ = tool_button(this, QStyle::SP_BrowserReload, tr("Loop (L)"));
    loop_->setCheckable(true);
    fullscreen_ = tool_button(this, QStyle::SP_TitleBarMaxButton, tr("Fullscreen (F, double-click the video)"));

    auto* bar = new QHBoxLayout;
    bar->setContentsMargins(4, 2, 4, 2);
    bar->addWidget(play_);
    bar->addWidget(restart_);
    bar->addWidget(position_, 1);
    bar->addWidget(time_);
    bar->addWidget(tracks_);
    bar->addWidget(mute_);
    bar->addWidget(volume_);
    bar->addWidget(loop_);
    bar->addWidget(fullscreen_);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(stack_, 1);
    layout->addLayout(bar);

    connect(play_, &QToolButton::clicked, this, &PlayerWidget::toggle_pause);
    connect(restart_, &QToolButton::clicked, this, &PlayerWidget::restart);
    connect(position_, &QSlider::sliderReleased, this, [this] { seek_to(position_->value() / 1000.0); });
    connect(position_, &QSlider::sliderMoved, this,
            [this](int v) { time_->setText(format_time(v / 1000.0) + " / " + format_time(setup_.duration)); });
    connect(tracks_, &QComboBox::activated, this, [this](int t) {
        if (src_ && sink_ && t != src_->audio_track()) {
            src_->select_audio_track(t, clock_.now());
            clock_.set(src_->seek_target());
        }
    });
    connect(mute_, &QToolButton::clicked, this, &PlayerWidget::toggle_mute);
    connect(volume_, &QSlider::valueChanged, this, [this] {
        if (updating_) return;
        muted_ = false;
        apply_volume();
    });
    connect(loop_, &QToolButton::toggled, this, &PlayerWidget::set_loop);
    connect(fullscreen_, &QToolButton::clicked, this, &PlayerWidget::fullscreen_requested);
    connect(view_, &VideoView::double_clicked, this, &PlayerWidget::fullscreen_requested);

    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(4);
    connect(&timer_, &QTimer::timeout, this, &PlayerWidget::tick);
    update_controls(true);
}

PlayerWidget::~PlayerWidget() { close_media(); }

void PlayerWidget::show_note(const QString& text) {
    note_->setText(text);
    stack_->setCurrentWidget(note_);
}

void PlayerWidget::close_media() {
    if (src_) final_stats_ = stats();
    timer_.stop();
    src_.reset();  // joins the worker before the sink goes
    sink_.reset();
    view_->clear();
    path_.clear();
    setup_ = {};
    ended_ = false;
    stats_ = {};
    tracks_->clear();
    update_controls(true);
}

bool PlayerWidget::open(const QString& path, const QStringList& track_labels, bool allow_non_bink) {
    close_media();
    PlayOptions opts;
    opts.input = path.toStdString();
    opts.allow_non_bink = allow_non_bink;
    auto src = MediaSource::open(opts);
    const QString name = QFileInfo(path).fileName();
    if (!src) {
        show_note(tr("Can't play %1:\n%2").arg(name, QString::fromStdString(src.error().message)));
        emit message(tr("Can't play %1").arg(name));
        return false;
    }
    setup_ = (*src)->setup();
    if (setup_.has_audio) {
        sink_ = QtAudioSink::open(setup_.sample_rate, setup_.channels);
        if (!sink_) emit message(tr("No audio output device; playing without sound"));
    }
    if (!setup_.has_video && !sink_) {
        show_note(tr("Can't play %1:\nno audio output, and no video to show.").arg(name));
        return false;
    }
    src_ = std::move(*src);
    path_ = path;

    if (setup_.has_video) {
        view_->set_sample_aspect(setup_.sample_aspect);
        stack_->setCurrentWidget(view_);
    } else {
        QString why = setup_.video_note.empty() ? tr("audio only") : QString::fromStdString(setup_.video_note);
        show_note(tr("%1\n\n%2").arg(name, why));
    }
    updating_ = true;
    tracks_->clear();
    for (int i = 0; i < setup_.audio_tracks; ++i)
        tracks_->addItem(i < track_labels.size() ? track_labels[i] : tr("Track %1").arg(i));
    tracks_->setCurrentIndex(std::max(setup_.audio_track, 0));
    updating_ = false;

    clock_ = SyncClock{};
    clock_.set(0);
    set_paused(false);
    apply_volume();
    src_->start(sink_.get());
    timer_.start();
    update_controls(true);
    return true;
}

void PlayerWidget::set_track_labels(const QStringList& labels) {
    for (int i = 0; i < tracks_->count() && i < labels.size(); ++i) tracks_->setItemText(i, labels[i]);
}

PlaybackStats PlayerWidget::stats() const {
    if (!src_) return final_stats_;
    PlaybackStats s = stats_;
    if (src_) {
        s.audio_clock = sink_ ? src_->audio_clock() : 0;
        s.track = src_->audio_track();
        s.rewinds = src_->rewinds();
    }
    return s;
}

double PlayerWidget::master_clock() {
    if (src_->seeking())
        clock_.set(src_->seek_target());
    else if (sink_ && !clock_.paused() && sink_->buffered_seconds() > 0)
        clock_.follow(src_->audio_clock());
    return clock_.now();
}

void PlayerWidget::tick() {
    if (!src_) return;
    const double now = master_clock();

    // Show the newest frame that is due; drop any that are already late.
    double pts0 = 0, pts1 = 0;
    FramePtr show;
    while (src_->peek_video(0, pts0) && pts0 <= now) {
        if (src_->peek_video(1, pts1) && pts1 <= now) {
            src_->pop_video();
            ++stats_.dropped;
            continue;
        }
        show = src_->pop_video();
        if (!show) break;
        if (stats_.shown == 0) stats_.first_pts = pts0;
        stats_.last_pts = pts0;
        ++stats_.shown;
    }
    if (show) view_->set_frame(std::move(show));

    // End of file: wait for the audio to drain and the last frame's duration.
    const bool video_done = !setup_.has_video || stats_.shown == 0 || setup_.fps <= 0 ||
                            now >= stats_.last_pts + 1 / setup_.fps;
    if (!ended_ && !clock_.paused() && src_->finished() && (!sink_ || sink_->buffered_seconds() < 0.005) &&
        video_done) {
        if (loop_on_) {
            seek_to(0);
        } else {
            ended_ = true;
            set_paused(true);
            update_controls(true);
            emit playback_finished();
            return;
        }
    }
    update_controls();
}

void PlayerWidget::update_controls(bool force) {
    const qint64 t = QDateTime::currentMSecsSinceEpoch();
    if (!force && t - last_ui_ms_ < 100) return;
    last_ui_ms_ = t;
    updating_ = true;
    const bool have = src_ != nullptr;
    play_->setEnabled(have);
    restart_->setEnabled(have);
    position_->setEnabled(have && setup_.duration > 0);
    tracks_->setEnabled(have && sink_ && setup_.audio_tracks > 1);
    tracks_->setVisible(setup_.audio_tracks > 0);
    mute_->setEnabled(have && sink_);
    volume_->setEnabled(have && sink_);
    const bool playing = have && !clock_.paused();
    play_->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    mute_->setIcon(style()->standardIcon(muted_ ? QStyle::SP_MediaVolumeMuted : QStyle::SP_MediaVolume));
    double now = have ? std::min(clock_.now(), setup_.duration > 0 ? setup_.duration : clock_.now()) : 0;
    if (ended_) now = setup_.duration;
    position_->setRange(0, static_cast<int>(setup_.duration * 1000));
    if (!position_->isSliderDown()) {
        position_->setValue(static_cast<int>(now * 1000));
        QString s = format_time(now) + " / " + format_time(setup_.duration);
        if (have && src_->seeking()) s += tr("  seeking…");
        time_->setText(s);
    }
    if (have && sink_ && tracks_->currentIndex() != src_->audio_track()) tracks_->setCurrentIndex(src_->audio_track());
    updating_ = false;
}

void PlayerWidget::set_paused(bool paused) {
    clock_.set_paused(paused);
    if (sink_) sink_->set_paused(paused);
}

void PlayerWidget::toggle_pause() {
    if (!src_) return;
    if (ended_) {
        restart();
        set_paused(false);
    } else {
        set_paused(!clock_.paused());
    }
    update_controls(true);
}

void PlayerWidget::seek_to(double seconds) {
    if (!src_) return;
    src_->seek(seconds);
    clock_.set(src_->seek_target());
    if (ended_) {
        ended_ = false;
        set_paused(false);
    }
    update_controls(true);
}

void PlayerWidget::seek_by(double seconds) {
    if (src_) seek_to(clock_.now() + seconds);
}

void PlayerWidget::restart() { seek_to(0); }

void PlayerWidget::step_track(int direction) {
    if (!src_ || !sink_ || setup_.audio_tracks < 2) return;
    const int n = setup_.audio_tracks;
    const int t = (src_->audio_track() + (direction < 0 ? n - 1 : 1)) % n;
    src_->select_audio_track(t, clock_.now());
    clock_.set(src_->seek_target());
    emit message(tr("Audio track: %1").arg(tracks_->itemText(t)));
    update_controls(true);
}

void PlayerWidget::apply_volume() {
    if (sink_) sink_->set_volume(muted_ ? 0.0f : static_cast<float>(volume_->value()) / 100.0f);
    update_controls(true);
}

void PlayerWidget::change_volume(int delta) {
    muted_ = false;
    volume_->setValue(std::clamp(volume_->value() + delta, 0, 100));  // applies through valueChanged
    apply_volume();
}

void PlayerWidget::toggle_mute() {
    muted_ = !muted_;
    apply_volume();
}

void PlayerWidget::set_loop(bool loop) {
    if (loop == loop_on_) return;
    loop_on_ = loop;
    loop_->setChecked(loop);
    emit loop_changed(loop);
}

void PlayerWidget::keyPressEvent(QKeyEvent* e) {
    // Up/Down change the volume only while the player has focus; elsewhere
    // they move through the file list.
    if (e->key() == Qt::Key_Up) {
        change_volume(10);
    } else if (e->key() == Qt::Key_Down) {
        change_volume(-10);
    } else {
        QWidget::keyPressEvent(e);
        return;
    }
    e->accept();
}

}  // namespace quillreel::gui
