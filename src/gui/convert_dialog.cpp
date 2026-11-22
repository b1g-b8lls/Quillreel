// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <format>

#include "bink2_dll.h"
#include "output_format.h"

namespace quillreel::gui {
namespace {

bool is_bink2(const ConvertItem& it) { return it.info && it.info->video && it.info->video->generation == "Bink 2"; }
bool bink2_video_enabled(const ConvertItem& it) { return is_bink2(it) && !it.info->video->decoder.empty(); }

const OutputFormat* format_by_ext(const QString& ext) {
    for (const OutputFormat& f : output_formats())
        if (QString::fromStdString(f.extension) == ext) return &f;
    return nullptr;
}

QString default_dir() {
    const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return !movies.isEmpty() && QDir(movies).exists() ? movies : QDir::homePath();
}

}  // namespace

ConvertDialog::ConvertDialog(std::vector<ConvertItem> items, bool allow_non_bink, QWidget* parent)
    : QDialog(parent), items_(std::move(items)), allow_non_bink_(allow_non_bink) {
    setWindowTitle(tr("Convert"));
    setMinimumWidth(620);
    QSettings settings;

    QStringList names;
    for (const ConvertItem& it : items_) names << QFileInfo(it.path).fileName();
    files_ = new QLabel(this);
    files_->setWordWrap(true);
    const int shown = 4;
    QString list = names.mid(0, shown).join(", ");
    if (names.size() > shown) list += tr(" and %n more", nullptr, static_cast<int>(names.size() - shown));
    files_->setText(tr("%n file(s): %1", nullptr, static_cast<int>(names.size())).arg(list));

    format_ = new QComboBox(this);
    auto* model = qobject_cast<QStandardItemModel*>(format_->model());
    for (const OutputFormat& f : output_formats()) {
        const bool ok = output_format_available(f);
        format_->addItem(QString::fromStdString(f.description) + (ok ? QString() : tr("  (not in this FFmpeg build)")),
                         QString::fromStdString(f.extension));
        if (!ok && model) model->item(format_->count() - 1)->setEnabled(false);
    }
    set_format(settings.value("convert/format", "mp4").toString());

    dir_ = new QLineEdit(settings.value("convert/dir", default_dir()).toString(), this);
    browse_ = new QPushButton(tr("Browse…"), this);
    auto* dir_row = new QHBoxLayout;
    dir_row->addWidget(dir_, 1);
    dir_row->addWidget(browse_);

    video_ = new QCheckBox(tr("Include video"), this);
    video_->setChecked(settings.value("convert/video", true).toBool());
    audio_ = new QComboBox(this);
    audio_->addItem(tr("First track"), kFirstTrack);
    audio_->addItem(tr("All tracks"), kAllTracks);
    audio_->addItem(tr("Track number:"), kOneTrack);
    audio_->addItem(tr("No audio"), kNoAudio);
    audio_->setCurrentIndex(std::clamp(settings.value("convert/audio", 0).toInt(), 0, 3));
    track_ = new QSpinBox(this);
    track_->setRange(0, 99);
    track_->setValue(settings.value("convert/track", 0).toInt());
    auto* audio_row = new QHBoxLayout;
    audio_row->addWidget(audio_);
    audio_row->addWidget(track_);
    audio_row->addStretch();
    overwrite_ = new QCheckBox(tr("Replace existing files"), this);

    // Quality, the same choices as `quillreel convert --quality/--crf/--speed/
    // --height/--audio-bitrate`.
    quality_box_ = new QGroupBox(tr("Quality"), this);
    quality_ = new QComboBox(quality_box_);
    quality_->addItem(tr("Best (near-lossless, default)"));
    quality_->addItem(tr("High"));
    quality_->addItem(tr("Medium"));
    quality_->addItem(tr("Small file"));
    quality_->addItem(tr("Custom CRF:"));
    quality_->setToolTip(tr("Video quality. Lower CRF means better quality and bigger files."));
    crf_ = new QSpinBox(quality_box_);
    crf_->setRange(0, 51);
    speed_ = new QComboBox(quality_box_);
    speed_->addItem(tr("Normal"));
    speed_->addItem(tr("Fast (bigger files)"));
    speed_->addItem(tr("Slow (smaller files)"));
    size_ = new QComboBox(quality_box_);
    size_->addItem(tr("Original"), 0);
    for (int h : {1080, 720, 480, 360}) size_->addItem(tr("%1p").arg(h), h);
    size_->addItem(tr("Custom height:"), -1);
    size_->setToolTip(tr("Output height; the width follows the aspect ratio."));
    height_ = new QSpinBox(quality_box_);
    height_->setRange(16, 4320);
    height_->setSingleStep(2);
    height_->setSuffix(tr(" px"));
    abitrate_ = new QComboBox(quality_box_);
    abitrate_->addItem(tr("Default (80 kb/s per channel)"), 0);
    for (int k : {64, 96, 128, 160, 192, 256, 320}) abitrate_->addItem(tr("%1 kb/s").arg(k), k);
    abitrate_->setToolTip(tr("Per audio track; only for lossy audio (AAC, Opus, Vorbis, MP3)."));
    auto row = [this](QWidget* a, QWidget* b) {
        auto* h = new QHBoxLayout;
        h->addWidget(a);
        if (b) h->addWidget(b);
        h->addStretch();
        return h;
    };
    auto* qform = new QFormLayout(quality_box_);
    qform->addRow(tr("Video quality:"), row(quality_, crf_));
    qform->addRow(tr("Encoding speed:"), row(speed_, nullptr));
    qform->addRow(tr("Size:"), row(size_, height_));
    qform->addRow(tr("Audio bitrate:"), row(abitrate_, nullptr));
    quality_->setCurrentIndex(std::clamp(settings.value("convert/quality", 0).toInt(), 0, 4));
    crf_->setValue(settings.value("convert/crf", 18).toInt());
    speed_->setCurrentIndex(std::clamp(settings.value("convert/speed", 0).toInt(), 0, 2));
    size_->setCurrentIndex(std::clamp(settings.value("convert/size", 0).toInt(), 0, size_->count() - 1));
    height_->setValue(settings.value("convert/height", 720).toInt());
    abitrate_->setCurrentIndex(std::clamp(settings.value("convert/abitrate", 0).toInt(), 0, abitrate_->count() - 1));

    notes_ = new QLabel(this);
    notes_->setWordWrap(true);
    notes_->setStyleSheet("color: palette(placeholder-text);");

    file_progress_ = new QProgressBar(this);
    file_progress_->setRange(0, 1000);
    file_progress_->setValue(0);
    file_progress_->setFormat(tr("Current file: %p%"));
    total_progress_ = new QProgressBar(this);
    total_progress_->setRange(0, 1000);
    total_progress_->setValue(0);
    total_progress_->setFormat(tr("All files: %p%"));
    log_ = new QPlainTextEdit(this);
    log_->setReadOnly(true);
    log_->setMinimumHeight(120);
    log_->setPlaceholderText(tr("Results appear here."));

    buttons_ = new QDialogButtonBox(QDialogButtonBox::Close, this);
    convert_ = buttons_->addButton(tr("&Convert"), QDialogButtonBox::AcceptRole);
    convert_->setDefault(true);
    cancel_button_ = buttons_->addButton(tr("C&ancel"), QDialogButtonBox::ActionRole);
    open_folder_ = buttons_->addButton(tr("&Open Folder"), QDialogButtonBox::ActionRole);
    cancel_button_->setEnabled(false);

    auto* form = new QFormLayout;
    form->addRow(tr("Files:"), files_);
    form->addRow(tr("Format:"), format_);
    form->addRow(tr("Save to:"), dir_row);
    form->addRow(tr("Video:"), video_);
    form->addRow(tr("Audio:"), audio_row);
    form->addRow(QString(), overwrite_);
    form->addRow(quality_box_);
    form->addRow(QString(), notes_);
    auto* layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(file_progress_);
    layout->addWidget(total_progress_);
    layout->addWidget(log_, 1);
    layout->addWidget(buttons_);

    connect(format_, &QComboBox::currentIndexChanged, this, &ConvertDialog::update_notes);
    connect(video_, &QCheckBox::toggled, this, &ConvertDialog::update_notes);
    connect(audio_, &QComboBox::currentIndexChanged, this, &ConvertDialog::update_notes);
    connect(track_, &QSpinBox::valueChanged, this, &ConvertDialog::update_notes);
    connect(dir_, &QLineEdit::textChanged, this, &ConvertDialog::update_notes);
    for (QComboBox* c : {quality_, speed_, size_, abitrate_})
        connect(c, &QComboBox::currentIndexChanged, this, &ConvertDialog::update_notes);
    connect(browse_, &QPushButton::clicked, this, &ConvertDialog::browse);
    connect(convert_, &QPushButton::clicked, this, &ConvertDialog::start);
    connect(cancel_button_, &QPushButton::clicked, this, &ConvertDialog::cancel);
    connect(open_folder_, &QPushButton::clicked, this,
            [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(dir_->text())); });
    connect(buttons_, &QDialogButtonBox::rejected, this, &ConvertDialog::reject);
    update_notes();
}

ConvertDialog::~ConvertDialog() {
    cancel_ = true;
    if (worker_.joinable()) worker_.join();
}

bool ConvertDialog::set_format(const QString& extension) {
    const int i = format_->findData(extension.toLower());
    if (i < 0) return false;
    format_->setCurrentIndex(i);
    return true;
}

void ConvertDialog::set_output_dir(const QString& dir) { dir_->setText(dir); }
void ConvertDialog::set_video(bool on) { video_->setChecked(on); }
void ConvertDialog::set_audio(int mode, int track) {
    audio_->setCurrentIndex(std::clamp(mode, 0, 3));
    track_->setValue(track);
}
void ConvertDialog::set_overwrite(bool on) { overwrite_->setChecked(on); }
void ConvertDialog::set_quality(int level, int crf) {
    quality_->setCurrentIndex(std::clamp(level, 0, 4));
    crf_->setValue(crf);
}
void ConvertDialog::set_speed(int speed) { speed_->setCurrentIndex(std::clamp(speed, 0, 2)); }
void ConvertDialog::set_height(int height) {
    const int i = size_->findData(height);
    if (height <= 0) {
        size_->setCurrentIndex(0);
    } else if (i >= 0) {
        size_->setCurrentIndex(i);
    } else {
        size_->setCurrentIndex(size_->findData(-1));
        height_->setValue(height);
    }
}
void ConvertDialog::set_audio_bitrate(int kbps) { abitrate_->setCurrentIndex(std::max(0, abitrate_->findData(kbps))); }
QString ConvertDialog::log_text() const { return log_->toPlainText(); }

void ConvertDialog::browse() {
    const QString d = QFileDialog::getExistingDirectory(this, tr("Save Converted Files To"), dir_->text());
    if (!d.isEmpty()) dir_->setText(d);
}

std::vector<ConvertDialog::Job> ConvertDialog::plan() const {
    std::vector<Job> jobs;
    const OutputFormat* f = format_by_ext(format_->currentData().toString());
    if (!f) return jobs;
    const bool fmt_video = !f->video_encoders.empty(), fmt_audio = !f->audio_encoders.empty();
    const auto mode = static_cast<AudioMode>(audio_->currentData().toInt());
    const QDir dir(dir_->text());
    const QString ext = QString::fromStdString(f->extension);
    QSet<QString> taken;

    for (const ConvertItem& it : items_) {
        Job j;
        j.name = QFileInfo(it.path).fileName();
        ConvertOptions& o = j.opts;
        o.input = it.path.toStdString();
        o.overwrite = overwrite_->isChecked();
        o.allow_non_bink = allow_non_bink_;
        const bool known = it.info.has_value();
        const bool has_video = !known || it.info->video.has_value();
        const int tracks = known ? static_cast<int>(it.info->audio.size()) : 1;

        o.video = video_->isChecked() && fmt_video && has_video;
        if (f->video_quality) {
            using Q = ConvertOptions::Quality;
            switch (quality_->currentIndex()) {
            case 1: o.quality = Q::High; break;
            case 2: o.quality = Q::Medium; break;
            case 3: o.quality = Q::Small; break;
            case 4: o.crf = crf_->value(); break;
            default: break;
            }
            o.speed = static_cast<ConvertOptions::Speed>(speed_->currentIndex());
        }
        if (fmt_video) {
            const int h = size_->currentData().toInt();
            o.height = h < 0 ? height_->value() : h;
        }
        if (f->audio_bitrate) o.audio_bitrate_kbps = abitrate_->currentData().toInt();
        if (o.video && is_bink2(it) && !bink2_video_enabled(it)) {
            o.video = false;
            j.note = tr("Bink 2 video is not enabled, so audio only");
        }
        o.audio = fmt_audio && mode != kNoAudio && tracks > 0;
        if (o.audio && mode == kAllTracks && tracks > 1) {
            if (f->multi_audio) {
                o.all_audio = true;
            } else if (j.note.isEmpty()) {
                j.note = tr("this format holds one track, so track 0 only");
            }
        }
        if (o.audio && mode == kOneTrack) {
            if (known && track_->value() >= tracks) {
                j.skip = tr("it has no audio track %1 (tracks 0-%2)").arg(track_->value()).arg(tracks - 1);
                jobs.push_back(std::move(j));
                continue;
            }
            o.audio_track = track_->value();
        }
        if (!o.video && !o.audio) {
            if (is_bink2(it) && !bink2_video_enabled(it) && fmt_video && !fmt_audio)
                j.skip = tr("Bink 2 video is not enabled (Help > Bink 2 Video)");
            else
                j.skip = tr("nothing to convert with these settings");
            jobs.push_back(std::move(j));
            continue;
        }

        // Output name: the input's name with the new extension, made unique in this batch.
        QString stem = QFileInfo(it.path).completeBaseName();
        QString unique = stem;
        for (int n = 2; taken.contains(unique.toLower()); ++n) unique = QString("%1 (%2)").arg(stem).arg(n);
        taken.insert(unique.toLower());
        QString first_file;
        if (f->image_sequence) {
            j.output = dir.filePath(unique + "/%05d." + ext);
            first_file = dir.filePath(unique + "/00001." + ext);
        } else {
            j.output = first_file = dir.filePath(unique + "." + ext);
        }
        if (!o.overwrite && QFileInfo::exists(first_file)) {
            j.skip = tr("%1 already exists (tick \"Replace existing files\")").arg(QFileInfo(first_file).fileName());
            j.output.clear();
        }
        o.output = j.output.toStdString();
        jobs.push_back(std::move(j));
    }
    return jobs;
}

void ConvertDialog::update_notes() {
    const OutputFormat* f = format_by_ext(format_->currentData().toString());
    if (!f) return;
    const bool fmt_video = !f->video_encoders.empty(), fmt_audio = !f->audio_encoders.empty();
    const auto mode = static_cast<AudioMode>(audio_->currentData().toInt());
    video_->setEnabled(fmt_video && !running());
    audio_->setEnabled(fmt_audio && !running());
    track_->setVisible(mode == kOneTrack);
    quality_->setEnabled(f->video_quality && !running());
    speed_->setEnabled(f->video_quality && !running());
    crf_->setVisible(quality_->currentIndex() == 4);
    crf_->setEnabled(f->video_quality && !running());
    crf_->setMaximum(f->extension == "webm" ? 63 : 51);  // VP9/AV1 vs H.264
    size_->setEnabled(fmt_video && !running());
    height_->setVisible(size_->currentData().toInt() < 0);
    height_->setEnabled(fmt_video && !running());
    abitrate_->setEnabled(f->audio_bitrate && !running());

    QStringList notes;
    if (!fmt_video) notes << tr("This format holds audio only.");
    if (!fmt_audio) notes << tr("This format holds no audio.");
    if (f->extension == "y4m") notes << tr("Y4M is uncompressed: about 3 MB per 1080p frame.");
    if (f->image_sequence) notes << tr("Each file gets its own folder of numbered PNG frames.");
    if (fmt_audio && mode == kAllTracks && !f->multi_audio)
        notes << tr("All tracks needs MP4, MOV, MKV, WebM or MKA; this format gets track 0 only.");
    int b2 = 0;
    for (const ConvertItem& it : items_)
        if (is_bink2(it) && !bink2_video_enabled(it)) ++b2;
    if (b2 && fmt_video && video_->isChecked())
        notes << (fmt_audio ? tr("%n Bink 2 file(s) will convert audio only: Bink 2 video is not enabled "
                                 "(Help > Bink 2 Video).",
                                 nullptr, b2)
                            : tr("%n Bink 2 file(s) will be skipped: Bink 2 video is not enabled "
                                 "(Help > Bink 2 Video).",
                                 nullptr, b2));
    if (!QDir(dir_->text()).exists()) notes << tr("The folder doesn't exist yet; it will be created.");
    notes_->setText(notes.join("\n"));
    notes_->setVisible(!notes.isEmpty());
}

void ConvertDialog::log(const QString& line) { log_->appendPlainText(line); }

void ConvertDialog::set_running(bool running) {
    for (QWidget* w : std::initializer_list<QWidget*>{format_, dir_, browse_, track_, overwrite_, convert_})
        w->setEnabled(!running);
    cancel_button_->setEnabled(running);
    update_notes();
}

void ConvertDialog::start() {
    if (running()) return;
    if (worker_.joinable()) worker_.join();
    if (dir_->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, tr("Convert"), tr("Choose a folder to save to."));
        return;
    }
    std::vector<Job> jobs = plan();
    QSettings settings;
    settings.setValue("convert/format", format_->currentData());
    settings.setValue("convert/dir", dir_->text());
    settings.setValue("convert/video", video_->isChecked());
    settings.setValue("convert/audio", audio_->currentIndex());
    settings.setValue("convert/track", track_->value());
    settings.setValue("convert/quality", quality_->currentIndex());
    settings.setValue("convert/crf", crf_->value());
    settings.setValue("convert/speed", speed_->currentIndex());
    settings.setValue("convert/size", size_->currentIndex());
    settings.setValue("convert/height", height_->value());
    settings.setValue("convert/abitrate", abitrate_->currentIndex());

    log_->clear();
    jobs_ = static_cast<int>(jobs.size());
    converted_ = skipped_ = failed_ = 0;
    file_progress_->setValue(0);
    total_progress_->setValue(0);
    cancel_ = false;
    set_running(true);

    worker_ = std::thread([this, jobs = std::move(jobs)]() mutable {
        for (int i = 0; i < static_cast<int>(jobs.size()); ++i) {
            Job& j = jobs[i];
            QString message;
            bool ok = false;
            if (cancel_) {
                message = tr("– %1: not converted (cancelled)").arg(j.name);
            } else if (!j.skip.isEmpty()) {
                message = tr("– %1: skipped, %2").arg(j.name, j.skip);
            } else {
                if (QFileInfo(j.output).dir().mkpath(".")) {
                    j.opts.cancel = &cancel_;
                    auto progress = [this, i](double done, double total) {
                        QMetaObject::invokeMethod(this, [this, i, done, total] { job_progress(i, done, total); },
                                                  Qt::QueuedConnection);
                    };
                    auto r = convert_file(j.opts, progress);
                    if (r) {
                        ok = true;
                        QString what;
                        if (r->video_frames >= 0) what = tr("%n frame(s)", nullptr, static_cast<int>(r->video_frames));
                        for (size_t t = 0; t < r->audio_tracks.size(); ++t) {
                            double secs = r->audio_rates[t] ? static_cast<double>(r->audio_samples[t]) / r->audio_rates[t] : 0;
                            what += (what.isEmpty() ? "" : ", ") +
                                    tr("audio track %1 (%2 s)").arg(r->audio_tracks[t]).arg(secs, 0, 'f', 1);
                        }
                        message = tr("✓ %1 → %2: %3").arg(j.name, QFileInfo(j.output).fileName(), what);
                        if (!j.note.isEmpty()) message += tr(" (%1)").arg(j.note);
                        if (r->decode_errors) message += tr(", %n damaged packet(s) skipped", nullptr, static_cast<int>(r->decode_errors));
                    } else if (cancel_) {
                        message = tr("– %1: cancelled").arg(j.name);
                    } else {
                        message = tr("✗ %1: %2").arg(j.name, QString::fromStdString(r.error().message));
                    }
                } else {
                    message = tr("✗ %1: cannot create %2").arg(j.name, QFileInfo(j.output).absolutePath());
                }
            }
            const bool skipped = !ok && (!j.skip.isEmpty() || message.startsWith("–"));
            QMetaObject::invokeMethod(
                this, [this, i, ok, skipped, message] {
                    if (skipped) ++skipped_;
                    job_done(i, ok, message);
                },
                Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this] { all_done(); }, Qt::QueuedConnection);
    });
}

void ConvertDialog::job_progress(int index, double done, double total) {
    const double frac = total > 0 ? std::clamp(done / total, 0.0, 1.0) : 0;
    file_progress_->setValue(static_cast<int>(frac * 1000));
    total_progress_->setValue(static_cast<int>((index + frac) / std::max(jobs_, 1) * 1000));
}

void ConvertDialog::job_done(int index, bool ok, const QString& message) {
    if (ok)
        ++converted_;
    else if (!message.startsWith("–"))
        ++failed_;
    log(message);
    file_progress_->setValue(ok ? 1000 : 0);
    total_progress_->setValue(static_cast<int>(static_cast<double>(index + 1) / std::max(jobs_, 1) * 1000));
}

void ConvertDialog::all_done() {
    if (worker_.joinable()) worker_.join();
    set_running(false);
    QString summary = tr("Done: %1 converted").arg(converted_);
    if (skipped_) summary += tr(", %1 skipped").arg(skipped_);
    if (failed_) summary += tr(", %1 failed").arg(failed_);
    if (cancel_) summary += tr(" (cancelled)");
    log(summary + ".");
    emit batch_finished(converted_, skipped_, failed_);
}

void ConvertDialog::cancel() {
    cancel_ = true;
    cancel_button_->setEnabled(false);
    log(tr("Cancelling…"));
}

void ConvertDialog::reject() {
    if (running()) {
        if (QMessageBox::question(this, tr("Convert"), tr("Stop converting? The file being written is deleted.")) !=
            QMessageBox::Yes)
            return;
        cancel_ = true;
        worker_.join();
    }
    QDialog::reject();
}

}  // namespace quillreel::gui
