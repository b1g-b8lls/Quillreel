// SPDX-License-Identifier: GPL-3.0-or-later
#include "main_window.h"

#include <QAction>
#include <QDateTime>
#include <QDesktopServices>
#include <QStandardPaths>
#include <QUrl>
#include <QCloseEvent>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QTextBrowser>
#include <QPointer>
#include <QSettings>
#include <QSplitter>
#include <QStatusBar>
#include <QThreadPool>
#include <QTreeWidget>

#include <format>
#include <sstream>

#include "bink2_dialog.h"
#include "bink2_dll.h"
#include "convert_dialog.h"
#include "file_details.h"
#include "handbrake.h"
#include "player_widget.h"
#include "plural.h"
#include "report.h"
#include "updater.h"

namespace quillreel::gui {
namespace {

enum Column { kName, kType, kLength, kVideo, kAudio, kSize, kColumns };
constexpr int kPathRole = Qt::UserRole;
constexpr int kSortRole = Qt::UserRole + 1;

// Sorts by the numeric value stored in kSortRole where there is one.
class FileItem final : public QTreeWidgetItem {
public:
    using QTreeWidgetItem::QTreeWidgetItem;
    bool operator<(const QTreeWidgetItem& other) const override {
        const int c = treeWidget() ? treeWidget()->sortColumn() : 0;
        const QVariant a = data(c, kSortRole), b = other.data(c, kSortRole);
        if (a.isValid() && b.isValid()) return a.toDouble() < b.toDouble();
        return text(c).localeAwareCompare(other.text(c)) < 0;
    }
};

bool is_bink_name(const QString& path) {
    const QString s = path.toLower();
    return s.endsWith(".bik") || s.endsWith(".bk2");
}

}  // namespace

MainWindow::MainWindow(const GuiOptions& opts, QWidget* parent) : QMainWindow(parent), opts_(opts) {
    setWindowTitle(tr("Quillreel"));
    setAcceptDrops(true);

    files_ = new QTreeWidget(this);
    files_->setColumnCount(kColumns);
    files_->setHeaderLabels({tr("Name"), tr("Type"), tr("Length"), tr("Video"), tr("Tracks"), tr("Size")});
    files_->setRootIsDecorated(false);
    files_->setUniformRowHeights(true);
    files_->setSortingEnabled(true);
    files_->sortByColumn(kName, Qt::AscendingOrder);
    files_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    files_->setAllColumnsShowFocus(true);
    files_->header()->setStretchLastSection(false);
    for (int c = 0; c < kColumns; ++c) files_->header()->setSectionResizeMode(c, QHeaderView::ResizeToContents);

    player_ = new PlayerWidget(this);

    info_ = new QTextBrowser(this);
    info_->setOpenLinks(false);
    info_->setPlaceholderText(tr("Select a file to see its details."));

    top_ = new QSplitter(Qt::Horizontal, this);
    top_->addWidget(files_);
    top_->addWidget(player_);
    top_->setStretchFactor(0, 0);
    top_->setStretchFactor(1, 1);
    top_->setSizes({560, 720});
    outer_ = new QSplitter(Qt::Vertical, this);
    outer_->addWidget(top_);
    outer_->addWidget(info_);
    outer_->setStretchFactor(0, 1);
    outer_->setStretchFactor(1, 0);
    outer_->setSizes({560, 170});
    setCentralWidget(outer_);

    scan_label_ = new QLabel(this);
    statusBar()->addPermanentWidget(scan_label_);

    updater_ = new Updater(this);
    build_menus();

    connect(files_, &QTreeWidget::itemSelectionChanged, this, &MainWindow::show_selected_info);
    connect(files_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) { activate(item); });
    connect(player_, &PlayerWidget::message, this, [this](const QString& m) { statusBar()->showMessage(m, 4000); });
    connect(player_, &PlayerWidget::fullscreen_requested, this, &MainWindow::toggle_fullscreen);
    connect(player_, &PlayerWidget::loop_changed, loop_action_, &QAction::setChecked);
    connect(player_, &PlayerWidget::bink2_setup_requested, this, &MainWindow::show_bink2_dialog);
    files_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(files_, &QWidget::customContextMenuRequested, this, &MainWindow::show_context_menu);

    QSettings settings;
    restoreGeometry(settings.value("window/geometry").toByteArray());
    outer_->restoreState(settings.value("window/outer").toByteArray());
    top_->restoreState(settings.value("window/top").toByteArray());
    if (!settings.contains("window/geometry")) resize(1280, 800);
}

MainWindow::~MainWindow() {
    // Scans post their results to this window; let them finish first.
    QThreadPool::globalInstance()->waitForDone();
}

void MainWindow::build_menus() {
    QMenu* file = menuBar()->addMenu(tr("&File"));
    file->addAction(QIcon::fromTheme("document-open"), tr("&Open Files…"), QKeySequence::Open, this,
                    &MainWindow::open_files);
    file->addAction(QIcon::fromTheme("folder-open"), tr("Open &Folder…"), QKeySequence(tr("Ctrl+Shift+O")), this,
                    &MainWindow::open_folder);
    file->addSeparator();
    file->addAction(QIcon::fromTheme("document-save-as"), tr("&Convert…"), QKeySequence(tr("Ctrl+E")), this,
                    &MainWindow::convert_selected);
    file->addAction(tr("Open in &HandBrake…"), this, [this] { open_in_handbrake(); });
    file->addSeparator();
    file->addAction(tr("&Clear List"), this, &MainWindow::clear_list);
    file->addSeparator();
    file->addAction(QIcon::fromTheme("application-exit"), tr("&Quit"), QKeySequence::Quit, this, &QWidget::close);

    // Playback keys work anywhere in the window (Up/Down stay with the file list).
    QMenu* play = menuBar()->addMenu(tr("&Playback"));
    auto add = [&](const QString& text, const QList<QKeySequence>& keys, auto fn) {
        QAction* a = play->addAction(text);
        a->setShortcuts(keys);
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, player_, fn);
        return a;
    };
    add(tr("Play / &Pause"), {QKeySequence(Qt::Key_Space), QKeySequence(Qt::Key_K)}, [this] { player_->toggle_pause(); });
    add(tr("&Restart"), {QKeySequence(Qt::Key_Home)}, [this] { player_->restart(); });
    add(tr("Go to &End"), {QKeySequence(Qt::Key_End)}, [this] { player_->seek_end(); });
    play->addSeparator();
    add(tr("Previous &Frame"), {QKeySequence(Qt::Key_Comma)}, [this] { player_->step_frame(-1); });
    add(tr("Ne&xt Frame"), {QKeySequence(Qt::Key_Period)}, [this] { player_->step_frame(1); });
    add(tr("Back 5 s"), {QKeySequence(Qt::Key_Left)}, [this] { player_->seek_by(-5); });
    add(tr("Forward 5 s"), {QKeySequence(Qt::Key_Right)}, [this] { player_->seek_by(5); });
    add(tr("Back 10 s"), {QKeySequence(Qt::Key_J)}, [this] { player_->seek_by(-10); });
    add(tr("Forward 10 s"), {QKeySequence(Qt::Key_L)}, [this] { player_->seek_by(10); });
    add(tr("Back 30 s"), {QKeySequence(Qt::SHIFT | Qt::Key_Left)}, [this] { player_->seek_by(-30); });
    add(tr("Forward 30 s"), {QKeySequence(Qt::SHIFT | Qt::Key_Right)}, [this] { player_->seek_by(30); });
    // 0-9 jump to 0-90 % (YouTube's keys), without cluttering the menu.
    for (int i = 0; i <= 9; ++i) {
        auto* a = new QAction(this);
        a->setShortcut(QKeySequence(Qt::Key_0 + i));
        a->setShortcutContext(Qt::WindowShortcut);
        connect(a, &QAction::triggered, player_, [this, i] { player_->seek_percent(i); });
        addAction(a);
    }
    play->addSeparator();
    add(tr("&Next Audio Track"), {QKeySequence(Qt::Key_A)}, [this] { player_->step_track(1); });
    add(tr("Pre&vious Audio Track"), {QKeySequence(Qt::SHIFT | Qt::Key_A)}, [this] { player_->step_track(-1); });
    add(tr("Volume Up"), {QKeySequence(Qt::CTRL | Qt::Key_Up)}, [this] { player_->change_volume(10); });
    add(tr("Volume Down"), {QKeySequence(Qt::CTRL | Qt::Key_Down)}, [this] { player_->change_volume(-10); });
    add(tr("&Mute"), {QKeySequence(Qt::Key_M)}, [this] { player_->toggle_mute(); });
    loop_action_ = add(tr("&Loop"), {QKeySequence(Qt::Key_R)}, [this](bool on) { player_->set_loop(on); });
    loop_action_->setCheckable(true);

    QMenu* view = menuBar()->addMenu(tr("&View"));
    QAction* fs = view->addAction(tr("&Fullscreen"), this, &MainWindow::toggle_fullscreen);
    fs->setShortcuts({QKeySequence(Qt::Key_F), QKeySequence(Qt::Key_F11)});
    QAction* esc = new QAction(this);
    esc->setShortcut(QKeySequence(Qt::Key_Escape));
    connect(esc, &QAction::triggered, this, [this] {
        if (isFullScreen()) toggle_fullscreen();
    });
    addAction(esc);
    view->addSeparator();
    show_list_ = view->addAction(tr("File &List"));
    show_list_->setCheckable(true);
    show_list_->setChecked(true);
    connect(show_list_, &QAction::toggled, files_, &QWidget::setVisible);
    show_info_ = view->addAction(tr("File &Info"));
    show_info_->setCheckable(true);
    show_info_->setChecked(true);
    connect(show_info_, &QAction::toggled, info_, &QWidget::setVisible);
    // The `quillreel inspect` report instead of the summary.
    technical_ = view->addAction(tr("&Technical Details"));
    technical_->setCheckable(true);
    technical_->setChecked(QSettings().value("view/technical_details", false).toBool());
    connect(technical_, &QAction::toggled, this, [this](bool on) {
        QSettings().setValue("view/technical_details", on);
        show_selected_info();
    });

    QMenu* help = menuBar()->addMenu(tr("&Help"));
    help->addAction(tr("&Bink 2 Video…"), this, &MainWindow::show_bink2_dialog);
    help->addSeparator();
    help->addAction(tr("Check for &Updates…"), updater_, &Updater::check_now);
    QAction* automatic = help->addAction(tr("Check for Updates Au&tomatically"));
    automatic->setCheckable(true);
    automatic->setChecked(updater_->automatic());
    connect(automatic, &QAction::toggled, updater_, &Updater::set_automatic);
    connect(updater_, &Updater::automatic_changed, automatic, [automatic](bool on) {
        const QSignalBlocker block(automatic);
        automatic->setChecked(on);
    });
    help->addSeparator();
    help->addAction(tr("&About Quillreel"), this, &MainWindow::show_about);
}

bool MainWindow::trigger_shortcut(const QKeySequence& keys) {
    for (QAction* a : findChildren<QAction*>()) {
        if (a->shortcuts().contains(keys) && a->isEnabled()) {
            a->trigger();
            return true;
        }
    }
    return false;
}

void MainWindow::add_paths(const QStringList& paths) {
    QStringList found;
    for (const QString& p : paths) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(fi.absoluteFilePath(), {"*.bik", "*.bk2", "*.BIK", "*.BK2"}, QDir::Files,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) found << it.next();
        } else if (fi.isFile()) {
            found << fi.absoluteFilePath();
        }
    }
    files_->setSortingEnabled(false);
    for (const QString& path : found) {
        if (items_.contains(path)) continue;
        auto* item = new FileItem(files_);
        item->setText(kName, QFileInfo(path).fileName());
        item->setToolTip(kName, path);
        item->setData(kName, kPathRole, path);
        item->setText(kType, tr("reading…"));
        items_.insert(path, item);
        scan(path);
    }
    files_->setSortingEnabled(true);
    if (pending_) scan_label_->setText(tr("Reading %1…").arg(count_of(pending_, tr("file"), tr("files"))));
    if (!found.isEmpty() && !files_->currentItem() && files_->topLevelItemCount())
        files_->setCurrentItem(files_->topLevelItem(0));
    if (!found.isEmpty()) {
        QSettings().setValue("files/last_dir", QFileInfo(found.first()).absolutePath());
    } else if (!paths.isEmpty()) {
        statusBar()->showMessage(tr("No Bink files (.bik, .bk2) found"), 4000);
    }
    if (!pending_) emit scan_finished();
}

// Reads one file in the background (the same as `quillreel inspect`).
void MainWindow::scan(const QString& path) {
    ++pending_;
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self, path] {
        auto r = std::make_shared<Result>(inspect_file(path.toStdString()));
        QMetaObject::invokeMethod(
            self.data(), [self, path, r] {
                if (self) self->set_result(path, *r);
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::set_result(const QString& path, const Result& r) {
    --pending_;
    scan_label_->setText(pending_ ? tr("Reading %1…").arg(count_of(pending_, tr("file"), tr("files"))) : QString());
    if (!items_.contains(path)) {  // the list was cleared meanwhile
        if (!pending_) emit scan_finished();
        return;
    }
    auto res = std::make_shared<Result>(r);
    results_.insert(path, res);
    QTreeWidgetItem* item = items_.value(path);
    if (item) {
        files_->setSortingEnabled(false);
        if (!r) {
            item->setText(kType, tr("error"));
            item->setToolTip(kType, QString::fromStdString(r.error().message));
            item->setForeground(kName, palette().brush(QPalette::Disabled, QPalette::Text));
        } else {
            const FileInfo& f = *r;
            item->setToolTip(kType, QString());
            QString type = f.video ? QString::fromStdString(f.video->generation) : tr("audio");
            if (f.video && f.video->generation == "Bink 2" && f.video->decoder.empty()) {
                type += tr(" (audio only)");
                item->setToolTip(kType, tr("Bink 2 video is not enabled. Provide bink2w64.dll: see Help > Bink 2 Video."));
            }
            item->setText(kType, type);
            item->setText(kLength, format_length(f.duration_s));
            item->setData(kLength, kSortRole, f.duration_s);
            if (f.video) {
                double fps = f.video->frame_rate.den ? static_cast<double>(f.video->frame_rate.num) / f.video->frame_rate.den : 0;
                item->setText(kVideo, QString("%1×%2  %3 fps").arg(f.video->width).arg(f.video->height).arg(fps, 0, 'g', 5));
                item->setData(kVideo, kSortRole, static_cast<double>(f.video->width) * f.video->height);
            }
            item->setText(kAudio, f.audio.empty() ? QString("–") : QString::number(f.audio.size()));
            item->setToolTip(kAudio, count_of(static_cast<qint64>(f.audio.size()), tr("audio track"), tr("audio tracks")));
            item->setTextAlignment(kAudio, Qt::AlignCenter);
            item->setData(kAudio, kSortRole, static_cast<double>(f.audio.size()));
            item->setText(kSize, format_size(f.file_size));
            item->setData(kSize, kSortRole, static_cast<double>(f.file_size));
        }
        files_->setSortingEnabled(true);
        if (item->isSelected()) show_selected_info();
    }
    if (r && QFileInfo(player_->current_path()).absoluteFilePath() == path) player_->set_track_labels(track_labels(path));
    if (!pending_) emit scan_finished();
}

void MainWindow::show_selected_info() {
    const auto sel = files_->selectedItems();
    if (sel.isEmpty()) {
        info_->clear();
        return;
    }
    const bool technical = technical_ && technical_->isChecked();
    info_->setFont(technical ? QFontDatabase::systemFont(QFontDatabase::FixedFont) : QFont());
    QString text;  // technical: plain text; otherwise HTML
    for (QTreeWidgetItem* item : sel) {
        const QString path = item->data(kName, kPathRole).toString();
        auto it = results_.find(path);
        if (technical) {
            if (!text.isEmpty()) text += "\n";
            if (it == results_.end()) {
                text += path + ": " + tr("reading…") + "\n";
            } else if (!**it) {
                text += path + ":\n  " + QString::fromStdString((*it)->error().message) + "\n";
            } else {
                std::ostringstream out;
                print_text(out, ***it);
                text += QString::fromStdString(out.str());
            }
        } else {
            if (!text.isEmpty()) text += "<hr>";
            if (it == results_.end())
                text += details_reading_html(path, palette());
            else if (!**it)
                text += details_error_html(path, QString::fromStdString((*it)->error().message), palette());
            else
                text += details_html(***it, palette());
        }
    }
    if (technical)
        info_->setPlainText(text);
    else
        info_->setHtml(text);
}

QStringList MainWindow::track_labels(const QString& path) const {
    QStringList labels;
    auto it = results_.find(path);
    if (it == results_.end() || !**it) return labels;
    const auto& audio = (*it)->value().audio;
    for (size_t i = 0; i < audio.size(); ++i) {
        const AudioTrack& a = audio[i];
        QString ch = a.channels == 1 ? tr("mono") : a.channels == 2 ? tr("stereo") : tr("%1 ch").arg(a.channels);
        labels << tr("Track %1  (%2 Hz %3)").arg(i).arg(a.sample_rate).arg(ch);
    }
    return labels;
}

void MainWindow::activate(QTreeWidgetItem* item) {
    if (item) play(item->data(kName, kPathRole).toString());
}

bool MainWindow::play(const QString& path) {
    if (QTreeWidgetItem* item = items_.value(QFileInfo(path).absoluteFilePath())) {
        files_->clearSelection();
        files_->setCurrentItem(item);
    }
    const bool ok = player_->open(path, track_labels(path), opts_.allow_non_bink);
    if (ok) setWindowTitle(tr("%1 - Quillreel").arg(QFileInfo(path).fileName()));
    player_->setFocus();
    return ok;
}

void MainWindow::open_files() {
    const QString dir = QSettings().value("files/last_dir", QDir::homePath()).toString();
    const QStringList picked =
        QFileDialog::getOpenFileNames(this, tr("Open Bink Files"), dir, tr("Bink files (*.bik *.bk2);;All files (*)"));
    if (picked.isEmpty()) return;
    add_paths(picked);
    if (picked.size() == 1) play(picked.first());
}

void MainWindow::open_folder() {
    const QString dir = QSettings().value("files/last_dir", QDir::homePath()).toString();
    const QString picked = QFileDialog::getExistingDirectory(this, tr("Open Folder of Bink Files"), dir);
    if (!picked.isEmpty()) add_paths({picked});
}

void MainWindow::clear_list() {
    files_->clear();
    items_.clear();
    results_.clear();
    info_->clear();
}

void MainWindow::toggle_fullscreen() {
    if (isFullScreen()) {
        showNormal();
        menuBar()->show();
        statusBar()->show();
        files_->setVisible(list_was_visible_);
        info_->setVisible(info_was_visible_);
    } else {
        list_was_visible_ = show_list_->isChecked();
        info_was_visible_ = show_info_->isChecked();
        files_->hide();
        info_->hide();
        menuBar()->hide();
        statusBar()->hide();
        showFullScreen();
    }
    player_->setFocus();
}

void MainWindow::show_bink2_dialog() {
    Bink2Dialog dialog(this);
    connect(&dialog, &Bink2Dialog::changed, this, &MainWindow::refresh_bink2);
    dialog.exec();
}

void MainWindow::refresh_bink2() {
    for (auto it = items_.begin(); it != items_.end(); ++it) {
        it.value()->setText(1, tr("reading…"));
        scan(it.key());
    }
    if (pending_) scan_label_->setText(tr("Reading %1…").arg(count_of(pending_, tr("file"), tr("files"))));
    // A playing .bk2 switches between audio only and video.
    const QString cur = player_->current_path();
    if (!cur.isEmpty() && cur.endsWith(".bk2", Qt::CaseInsensitive)) {
        const double at = player_->position();
        player_->open(cur, track_labels(cur), opts_.allow_non_bink, at);
    }
    statusBar()->showMessage(bink2::dll_path() ? tr("Bink 2 video enabled") : tr("Bink 2 video disabled"), 4000);
}

void MainWindow::select_all() { files_->selectAll(); }

void MainWindow::open_in_handbrake(bool autostart) {
    if (pending_) {
        statusBar()->showMessage(tr("Still reading files; try again in a moment"), 3000);
        return;
    }
    const std::optional<HandBrake> hb = find_handbrake();
    if (!hb) {
        QMessageBox box(QMessageBox::Information, tr("Open in HandBrake"),
                        tr("<p>HandBrake isn't installed (or Quillreel can't find it).</p>"
                           "<p>HandBrake is a free video converter with many presets. Get it from "
                           "<a href=\"https://handbrake.fr\">handbrake.fr</a>, or on Linux from your "
                           "distribution or Flathub, then try again.</p>"),
                        QMessageBox::Ok, this);
        box.setTextFormat(Qt::RichText);
        box.setTextInteractionFlags(Qt::TextBrowserInteraction);
        box.exec();
        return;
    }
    ConvertDialog* d = make_convert_dialog();
    if (!d) {
        QMessageBox::information(this, tr("Open in HandBrake"), tr("Add some Bink files first (File > Open)."));
        return;
    }
    // HandBrake can't read Bink, so Quillreel first writes an exact copy it can read.
    QSettings settings;
    QString base = settings.value("handbrake/dir").toString();
    if (base.isEmpty()) {
        const QString movies = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
        base = QDir(movies.isEmpty() ? QDir::homePath() : movies).filePath(tr("Quillreel for HandBrake"));
    }
    // Several files go to a folder of their own: HandBrake then scans just these.
    const int count = files_->selectedItems().isEmpty() ? files_->topLevelItemCount()
                                                        : static_cast<int>(files_->selectedItems().size());
    const QString dir = count > 1 ? QDir(base).filePath(QDateTime::currentDateTime().toString("yyyy-MM-dd hh.mm.ss")) : base;
    d->set_format("mkv-lossless");
    d->set_audio(1);  // all tracks
    d->set_output_dir(dir);
    d->set_purpose(tr("Open in HandBrake"), tr("&Convert and Open"));
    d->setAttribute(Qt::WA_DeleteOnClose);
    connect(d, &ConvertDialog::batch_finished, this, [this, d, hb = *hb, base](int converted, int, int) {
        const QStringList files = d->converted_files();
        if (!converted || files.isEmpty()) return;
        const QString folder = QFileInfo(files.front()).absolutePath();
        QSettings().setValue("handbrake/dir", base);
        QString error;
        if (!gui::open_in_handbrake(hb, files, folder, &error)) {
            QMessageBox::warning(this, tr("Open in HandBrake"), tr("HandBrake didn't start: %1.").arg(error));
            return;
        }
        statusBar()->showMessage(tr("Opened in HandBrake"), 5000);
        if (!hb.takes_source || (hb.flatpak && files.size() > 1))
            QMessageBox::information(
                d, tr("Open in HandBrake"),
                hb.takes_source
                    ? tr("HandBrake has the first file. Add the others from the folder that opened (File > Open Source, "
                         "or drag them in).")
                    : tr("HandBrake is starting. Drag the converted files from the folder that opened into HandBrake "
                         "(or use its Open Source button)."));
    });
    if (autostart) {
        d->show();
        d->start();
    } else {
        d->open();
    }
}

ConvertDialog* MainWindow::make_convert_dialog() {
    QList<QTreeWidgetItem*> sel = files_->selectedItems();
    if (sel.isEmpty())
        for (int i = 0; i < files_->topLevelItemCount(); ++i) sel << files_->topLevelItem(i);
    std::vector<ConvertItem> items;
    for (QTreeWidgetItem* item : sel) {
        const QString path = item->data(kName, kPathRole).toString();
        ConvertItem ci{path, std::nullopt};
        if (auto it = results_.find(path); it != results_.end() && **it) ci.info = ***it;
        items.push_back(std::move(ci));
    }
    if (items.empty()) return nullptr;
    return new ConvertDialog(std::move(items), opts_.allow_non_bink, this);
}

void MainWindow::convert_selected() {
    if (pending_) {
        statusBar()->showMessage(tr("Still reading files; try again in a moment"), 3000);
        return;
    }
    ConvertDialog* d = make_convert_dialog();
    if (!d) {
        QMessageBox::information(this, tr("Convert"), tr("Add some Bink files first (File > Open)."));
        return;
    }
    d->setAttribute(Qt::WA_DeleteOnClose);
    d->open();
}

void MainWindow::show_context_menu(const QPoint& pos) {
    QTreeWidgetItem* item = files_->itemAt(pos);
    if (!item) return;
    if (!item->isSelected()) {
        files_->clearSelection();
        item->setSelected(true);
    }
    const QString path = item->data(kName, kPathRole).toString();
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme("media-playback-start"), tr("&Play"), this, [this, path] { play(path); });
    menu.addAction(QIcon::fromTheme("document-save-as"), tr("&Convert…"), this, &MainWindow::convert_selected);
    menu.addAction(tr("Open in &HandBrake…"), this, [this] { open_in_handbrake(); });
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme("folder-open"), tr("Open Containing &Folder"), this, [path] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
    });
    menu.addAction(QIcon::fromTheme("list-remove"), tr("&Remove from List"), this, &MainWindow::remove_selected);
    menu.exec(files_->viewport()->mapToGlobal(pos));
}

void MainWindow::remove_selected() {
    for (QTreeWidgetItem* item : files_->selectedItems()) {
        const QString path = item->data(kName, kPathRole).toString();
        items_.remove(path);
        results_.remove(path);
        delete item;
    }
    show_selected_info();
}

void MainWindow::show_about() {
    QMessageBox::about(this, tr("About Quillreel"),
                       tr("<p><b>Quillreel</b></p><p>Inspect, play and convert Bink video files (.bik, .bk2). "
                          "Built on FFmpeg and Qt.</p><p>Not affiliated with RAD Game Tools or Epic Games."
                          "</p><p>License: GPL-3.0-or-later</p>"));
}

void MainWindow::closeEvent(QCloseEvent* e) {
    QSettings settings;
    if (isFullScreen()) toggle_fullscreen();
    settings.setValue("window/geometry", saveGeometry());
    settings.setValue("window/outer", outer_->saveState());
    settings.setValue("window/top", top_->saveState());
    player_->close_media();
    e->accept();
}

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
    if (e->mimeData()->hasUrls()) e->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* e) {
    QStringList paths;
    for (const QUrl& u : e->mimeData()->urls())
        if (u.isLocalFile()) paths << u.toLocalFile();
    add_paths(paths);
    if (paths.size() == 1 && is_bink_name(paths.first())) play(QFileInfo(paths.first()).absoluteFilePath());
    e->acceptProposedAction();
}

}  // namespace quillreel::gui
