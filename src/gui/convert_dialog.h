// SPDX-License-Identifier: GPL-3.0-or-later
// File > Convert: converts the selected files with the same code as
// `quillreel convert`, one after another on a worker thread. Spec: specs/gui.md
#pragma once

#include <QDialog>
#include <QString>

#include <atomic>
#include <optional>
#include <thread>
#include <vector>

#include "convert.h"
#include "output_format.h"
#include "inspect.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace quillreel::gui {

struct ConvertItem {
    QString path;
    std::optional<FileInfo> info;  // empty if the file couldn't be read
};

class ConvertDialog final : public QDialog {
    Q_OBJECT
public:
    ConvertDialog(std::vector<ConvertItem> items, bool allow_non_bink, QWidget* parent = nullptr);
    ~ConvertDialog() override;

    // For tests and callers that preset the dialog.
    bool set_format(const QString& extension);
    void set_output_dir(const QString& dir);
    void set_video(bool on);
    void set_audio(int mode, int track = 0);  // 0 first track, 1 all, 2 track number, 3 none
    void set_overwrite(bool on);
    // Quality: level 0 default, 1 high, 2 medium, 3 small, 4 custom CRF;
    // speed 0 normal, 1 fast, 2 slow; height 0 = original; kbps 0 = default.
    void set_quality(int level, int crf = 18);
    void set_speed(int speed);
    void set_height(int height);
    void set_audio_bitrate(int kbps);
    // Codec names as in `quillreel convert --video-codec/--audio-codec`; empty = unchanged.
    void set_codecs(const QString& video, const QString& audio);
    // Only part of each file (seconds); to <= 0 means to the end.
    void set_trim(double from, double to);
    QString log_text() const;
    void start();
    void cancel();
    bool running() const { return worker_.joinable(); }

signals:
    void batch_finished(int converted, int skipped, int failed);

protected:
    void reject() override;  // Close/Esc: asks before cancelling a running batch

private:
    enum AudioMode { kFirstTrack, kAllTracks, kOneTrack, kNoAudio };
    struct Job {
        QString name;          // input file name, for the log
        QString output;        // empty if skipped
        QString skip;          // why it is skipped
        QString note;          // what was adjusted, e.g. "audio only"
        ConvertOptions opts;
    };

    std::vector<Job> plan() const;
    // The chosen format with the codec choices applied (which options apply).
    std::optional<OutputFormat> effective_format() const;
    bool lossless() const;
    void fill_codecs();  // after a format change
    void update_notes();
    void browse();
    void job_progress(int index, double done, double total);
    void job_done(int index, bool ok, const QString& message);
    void all_done();
    void set_running(bool running);
    void log(const QString& line);

    std::vector<ConvertItem> items_;
    bool allow_non_bink_ = false;

    QLabel* files_ = nullptr;
    QComboBox* format_ = nullptr;
    QLineEdit* dir_ = nullptr;
    QPushButton* browse_ = nullptr;
    QCheckBox* video_ = nullptr;
    QComboBox* audio_ = nullptr;
    QSpinBox* track_ = nullptr;
    QCheckBox* overwrite_ = nullptr;
    QGroupBox* quality_box_ = nullptr;
    QComboBox* quality_ = nullptr;
    QSpinBox* crf_ = nullptr;
    QComboBox* speed_ = nullptr;
    QComboBox* size_ = nullptr;
    QSpinBox* height_ = nullptr;
    QComboBox* abitrate_ = nullptr;
    QWidget* codec_row_ = nullptr;
    QComboBox* vcodec_ = nullptr;
    QComboBox* acodec_ = nullptr;
    QCheckBox* trim_ = nullptr;
    QLineEdit* from_ = nullptr;
    QLineEdit* to_ = nullptr;
    QLabel* notes_ = nullptr;
    QProgressBar* file_progress_ = nullptr;
    QProgressBar* total_progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;
    QPushButton* convert_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* open_folder_ = nullptr;

    std::thread worker_;
    std::atomic<bool> cancel_{false};
    int jobs_ = 0, converted_ = 0, skipped_ = 0, failed_ = 0;
};

}  // namespace quillreel::gui
