// SPDX-License-Identifier: GPL-3.0-or-later
// The built-in player: video area plus transport controls, driving the shared
// playback core (MediaSource) from a Qt timer. Spec: specs/gui.md
#pragma once

#include <QString>
#include <QStringList>
#include <QTimer>
#include <QWidget>

#include <memory>

#include "player.h"
#include "sdl_audio_sink.h"
#include "sync_clock.h"

class QComboBox;
class QLabel;
class QPushButton;
class QSlider;
class QStackedWidget;
class QToolButton;

namespace quillreel::gui {

class SeekSlider;
class VideoView;

struct PlaybackStats {
    long long shown = 0, dropped = 0;
    double first_pts = -1, last_pts = -1, audio_clock = 0;
    int track = -1, rewinds = 0;
};

class PlayerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit PlayerWidget(QWidget* parent = nullptr);
    ~PlayerWidget() override;

    // Opens a file and starts playing it (or shows why it can't). track_labels
    // name the audio tracks in the track menu; numbers are used if empty.
    bool open(const QString& path, const QStringList& track_labels = {}, bool allow_non_bink = false,
              double start = 0);
    void close_media();
    void set_track_labels(const QStringList& labels);  // renames the track menu entries
    bool has_media() const { return src_ != nullptr; }
    QString current_path() const { return path_; }
    double position() const { return clock_.now(); }
    bool showing_video() const { return src_ && setup_.has_video; }
    PlaybackStats stats() const;  // of the current file, or the last one if closed

public slots:
    void toggle_pause();
    void seek_by(double seconds);
    void seek_to(double seconds);
    void restart();
    void seek_end();
    void seek_percent(int tenths);   // 0-9: 0-90 % of the file
    void step_frame(int direction);  // pauses, then shows the next (+1) or previous (-1) frame
    void step_track(int direction);  // +1 next, -1 previous
    void change_volume(int delta);
    void toggle_mute();
    void set_loop(bool loop);

signals:
    void fullscreen_requested();
    void bink2_setup_requested();  // the "Enable Bink 2 Video" button
    void loop_changed(bool loop);
    void playback_finished();  // reached the end without looping
    void message(const QString& text);

protected:
    void keyPressEvent(QKeyEvent* e) override;
    void changeEvent(QEvent* e) override;

private:
    void tick();
    double master_clock();
    void update_controls(bool force = false);
    void apply_volume();
    void set_paused(bool paused);
    void show_note(const QString& text, bool offer_bink2 = false);
    void refresh_icons();  // after a palette or style change

    VideoView* view_ = nullptr;
    QLabel* note_ = nullptr;
    QWidget* note_page_ = nullptr;
    QPushButton* enable_bink2_ = nullptr;
    QStackedWidget* stack_ = nullptr;
    QToolButton* play_ = nullptr;
    QToolButton* restart_ = nullptr;
    SeekSlider* position_ = nullptr;
    QLabel* time_ = nullptr;
    QComboBox* tracks_ = nullptr;
    QToolButton* mute_ = nullptr;
    QSlider* volume_ = nullptr;
    QToolButton* loop_ = nullptr;
    QToolButton* fullscreen_ = nullptr;
    QTimer timer_;
    QIcon icon_play_, icon_pause_, icon_volume_, icon_muted_;

    // The worker inside src_ writes to sink_, so src_ is declared after it
    // (destroyed first).
    std::unique_ptr<SdlAudioSink> sink_;
    std::unique_ptr<MediaSource> src_;
    StreamSetup setup_;
    QString path_;
    SyncClock clock_;
    bool ended_ = false;
    bool muted_ = false;
    bool loop_on_ = false;
    bool updating_ = false;  // controls are being set from code
    qint64 last_ui_ms_ = 0;
    PlaybackStats stats_;
    PlaybackStats final_stats_;  // of the last file, kept after close_media()
};

}  // namespace quillreel::gui
