// SPDX-License-Identifier: GPL-3.0-or-later
// Updates Quillreel itself from its GitHub releases. The AppImage replaces its
// own file and the Windows installer build runs the new installer; other builds
// (.deb, built from source) only say that a new version exists.
// Spec: specs/gui.md, "Updates".
#pragma once

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVersionNumber>
#include <expected>
#include <functional>

class QProgressDialog;
class QWidget;

namespace quillreel::gui {

enum class InstallKind {
    Notify,            // .deb, source build, unpacked Windows folder: no self-update
    AppImage,          // $APPIMAGE: the file is replaced
    WindowsInstaller,  // installed by the setup program: its successor is run
};

struct Release {
    QVersionNumber version;
    QString tag;
    QString notes;   // Markdown
    QUrl page;       // the release on GitHub
    QString asset;   // empty when there is nothing this build can install
    QUrl asset_url;
    QByteArray sha256;  // as published by GitHub; required to install
    qint64 size = 0;
};

// Reads GitHub's "latest release" JSON and picks the file for this kind of build.
// asset_prefix: download URLs must start with it.
std::expected<Release, QString> parse_release(const QByteArray& json, InstallKind kind, const QString& asset_prefix);

class Updater final : public QObject {
    Q_OBJECT
public:
    explicit Updater(QWidget* window);

    static InstallKind install_kind();

    // Help > Check for Updates: reports every outcome, including "up to date".
    void check_now();
    // At start-up: asks once whether to check automatically, then checks at
    // most once a day if allowed. Only new versions not skipped are reported.
    void start_automatic();
    bool automatic() const;
    void set_automatic(bool on);

    // Testing (--test-update): no dialogs; prints the outcome and, with
    // install, downloads, verifies and installs without restarting.
    void run_test(bool install, std::function<void(int)> done);

signals:
    void automatic_changed(bool on);

private:
    void fetch(std::function<void(std::expected<Release, QString>)> then);
    void offer(const Release& r);
    // Downloads and verifies the release's file; gives its path. progress may be null.
    void download(const Release& r, QProgressDialog* progress, std::function<void(std::expected<QString, QString>)> then);
    std::expected<void, QString> install(const QString& file);

    QWidget* window_;
    QNetworkAccessManager net_;
    bool busy_ = false;
};

}  // namespace quillreel::gui
