// SPDX-License-Identifier: GPL-3.0-or-later
// Main window: a list of Bink files, an info panel for the selected file, and
// the built-in player. Spec: specs/gui.md
#pragma once

#include <QHash>
#include <QMainWindow>
#include <QStringList>

#include <expected>
#include <memory>

#include "errors.h"
#include "inspect.h"

class QAction;
class QLabel;
class QPlainTextEdit;
class QSplitter;
class QTreeWidget;
class QTreeWidgetItem;

namespace quillreel::gui {

class PlayerWidget;

struct GuiOptions {
    bool allow_non_bink = false;  // testing: accept any FFmpeg-readable input
};

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const GuiOptions& opts, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Adds files, and the Bink files inside folders (recursively), to the list.
    void add_paths(const QStringList& paths);
    bool play(const QString& path);
    PlayerWidget* player() const { return player_; }
    QTreeWidget* file_list() const { return files_; }
    int pending_scans() const { return pending_; }
    // Triggers the menu action whose shortcut matches (for tests).
    bool trigger_shortcut(const QKeySequence& keys);

signals:
    void scan_finished();

protected:
    void closeEvent(QCloseEvent* e) override;
    void dragEnterEvent(QDragEnterEvent* e) override;
    void dropEvent(QDropEvent* e) override;

private:
    using Result = std::expected<FileInfo, Error>;

    void build_menus();
    void open_files();
    void open_folder();
    void clear_list();
    void show_selected_info();
    void activate(QTreeWidgetItem* item);
    void set_result(const QString& path, const Result& r);
    void toggle_fullscreen();
    void show_bink2_status();
    void show_about();
    QStringList track_labels(const QString& path) const;

    GuiOptions opts_;
    QTreeWidget* files_ = nullptr;
    QPlainTextEdit* info_ = nullptr;
    PlayerWidget* player_ = nullptr;
    QSplitter* outer_ = nullptr;  // (list | player) over info
    QSplitter* top_ = nullptr;    // list | player
    QLabel* scan_label_ = nullptr;
    QAction* show_list_ = nullptr;
    QAction* show_info_ = nullptr;
    QAction* loop_action_ = nullptr;
    QHash<QString, QTreeWidgetItem*> items_;
    QHash<QString, std::shared_ptr<Result>> results_;
    int pending_ = 0;
    bool list_was_visible_ = true, info_was_visible_ = true;
};

}  // namespace quillreel::gui
