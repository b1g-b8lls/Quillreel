// SPDX-License-Identifier: GPL-3.0-or-later
#include "updater.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QWidget>
#include <cstdio>
#include <filesystem>
#include <memory>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

namespace quillreel::gui {
namespace {

constexpr char kApi[] = "https://api.github.com/repos/b1g-b8lls/Quillreel/releases/latest";
constexpr char kAssetPrefix[] = "https://github.com/b1g-b8lls/Quillreel/releases/download/";
constexpr qint64 kCheckInterval = 20 * 3600;  // seconds; "at most once a day", with some slack

// QUILLREEL_UPDATE_API points the updater at a test server (tests/test_gui.py).
// Downloads must then come from that server.
QUrl api_url() {
    const QString test = qEnvironmentVariable("QUILLREEL_UPDATE_API");
    return QUrl(test.isEmpty() ? QString(kApi) : test);
}

QString asset_prefix() {
    const QString test = qEnvironmentVariable("QUILLREEL_UPDATE_API");
    if (test.isEmpty()) return kAssetPrefix;
    const QUrl u(test);
    return u.scheme() + "://" + u.authority() + "/";
}

QVersionNumber current_version() { return QVersionNumber::fromString(QApplication::applicationVersion()); }

QString megabytes(qint64 bytes) { return QString::number(bytes / 1048576.0, 'f', 1); }

}  // namespace

std::expected<Release, QString> parse_release(const QByteArray& json, InstallKind kind, const QString& prefix) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
    if (!doc.isObject()) return std::unexpected(QString("GitHub sent an unreadable reply (%1)").arg(err.errorString()));
    const QJsonObject o = doc.object();
    Release r;
    r.tag = o["tag_name"].toString();
    QString v = r.tag.startsWith('v') ? r.tag.mid(1) : r.tag;
    qsizetype suffix = 0;
    r.version = QVersionNumber::fromString(v, &suffix);
    if (r.version.isNull() || suffix != v.size())
        return std::unexpected(QString("the latest release has no version number (tag \"%1\")").arg(r.tag));
    r.notes = o["body"].toString();
    r.page = QUrl(o["html_url"].toString());

    QString wanted;
    if (kind == InstallKind::AppImage) wanted = QString("Quillreel-%1-x86_64.AppImage").arg(v);
    if (kind == InstallKind::WindowsInstaller) wanted = QString("Quillreel-%1-windows-x86_64-setup.exe").arg(v);
    if (wanted.isEmpty()) return r;
    for (const QJsonValue& a : o["assets"].toArray()) {
        const QJsonObject asset = a.toObject();
        if (asset["name"].toString() != wanted) continue;
        const QUrl url(asset["browser_download_url"].toString());
        const QString digest = asset["digest"].toString();
        const QByteArray sha = QByteArray::fromHex(digest.mid(7).toLatin1());
        // Without GitHub's checksum the download can't be checked: don't offer it.
        if (!url.toString().startsWith(prefix) || !digest.startsWith("sha256:") || sha.size() != 32) break;
        r.asset = wanted;
        r.asset_url = url;
        r.sha256 = sha;
        r.size = asset["size"].toInteger();
        break;
    }
    return r;
}

Updater::Updater(QWidget* window) : QObject(window), window_(window) {}

InstallKind Updater::install_kind() {
    if (!qEnvironmentVariable("APPIMAGE").isEmpty()) return InstallKind::AppImage;
#ifdef _WIN32
    // The installer puts its uninstaller next to the programs.
    if (QFile::exists(QApplication::applicationDirPath() + "/uninstall.exe")) return InstallKind::WindowsInstaller;
#endif
    return InstallKind::Notify;
}

bool Updater::automatic() const { return QSettings().value("updates/automatic", false).toBool(); }

void Updater::set_automatic(bool on) {
    QSettings().setValue("updates/automatic", on);
    emit automatic_changed(on);
}

void Updater::fetch(std::function<void(std::expected<Release, QString>)> then) {
    QNetworkRequest req(api_url());
    req.setHeader(QNetworkRequest::UserAgentHeader, "Quillreel/" + QApplication::applicationVersion());
    req.setRawHeader("Accept", "application/vnd.github+json");
    req.setTransferTimeout(30000);
    QNetworkReply* reply = net_.get(req);
    connect(reply, &QNetworkReply::finished, this, [reply, then = std::move(then)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 404) return then(std::unexpected(QString("no Quillreel release was found on GitHub")));
        if (reply->error() != QNetworkReply::NoError)
            return then(std::unexpected(QString("cannot reach GitHub: %1").arg(reply->errorString())));
        then(parse_release(reply->readAll(), install_kind(), asset_prefix()));
    });
}

void Updater::check_now() {
    if (busy_) return;
    busy_ = true;
    fetch([this](std::expected<Release, QString> r) {
        busy_ = false;
        if (!r) {
            QMessageBox::warning(window_, tr("Check for Updates"), tr("Couldn't check for updates: %1.").arg(r.error()));
            return;
        }
        QSettings().setValue("updates/last_check", QDateTime::currentDateTimeUtc());
        if (r->version <= current_version()) {
            QMessageBox::information(window_, tr("Check for Updates"),
                                     tr("Quillreel %1 is the latest version.").arg(current_version().toString()));
            return;
        }
        offer(*r);
    });
}

void Updater::start_automatic() {
    QTimer::singleShot(2000, this, [this] {
        QSettings settings;
        if (!settings.contains("updates/automatic")) {
            const auto answer = QMessageBox::question(
                window_, tr("Automatic Updates"),
                tr("<p>Should Quillreel check for new versions automatically?</p>"
                   "<p>It would look at Quillreel's releases on GitHub at most once a day, which "
                   "shares your IP address with GitHub. Nothing is installed without asking you.</p>"
                   "<p>You can change this later in the Help menu.</p>"));
            set_automatic(answer == QMessageBox::Yes);
        }
        if (!automatic() || busy_) return;
        const QDateTime last = settings.value("updates/last_check").toDateTime();
        if (last.isValid() && last.secsTo(QDateTime::currentDateTimeUtc()) < kCheckInterval) return;
        busy_ = true;
        fetch([this](std::expected<Release, QString> r) {
            busy_ = false;
            if (!r) {
                std::fprintf(stderr, "quillreel-gui: update check failed: %s\n", qPrintable(r.error()));
                return;
            }
            QSettings s;
            s.setValue("updates/last_check", QDateTime::currentDateTimeUtc());
            if (r->version > current_version() && s.value("updates/skip").toString() != r->version.toString())
                offer(*r);
        });
    });
}

void Updater::offer(const Release& r) {
    QMessageBox box(window_);
    box.setWindowTitle(tr("Update Available"));
    box.setIcon(QMessageBox::Information);
    box.setText(tr("<b>Quillreel %1 is available.</b> You have %2.")
                    .arg(r.version.toString(), current_version().toString()));
    const InstallKind kind = install_kind();
    QPushButton* install_button = nullptr;
    QPushButton* page = nullptr;
    if (!r.asset.isEmpty()) {
        box.setInformativeText(
            kind == InstallKind::AppImage
                ? tr("Quillreel will download the new AppImage (%1 MB), check it, and replace this one with it.")
                      .arg(megabytes(r.size))
                : tr("Quillreel will download the installer (%1 MB), check it, then close and run it. Windows will ask "
                     "for permission to install.")
                      .arg(megabytes(r.size)));
        install_button = box.addButton(tr("&Install Update"), QMessageBox::AcceptRole);
    } else {
        box.setInformativeText(
            kind == InstallKind::Notify
                ? tr("This copy of Quillreel can't update itself (it was installed by your package manager or built "
                     "from source). Get the new version from the release page.")
                : tr("The release has no download this copy can install. Get it from the release page."));
        page = box.addButton(tr("&Open Release Page"), QMessageBox::AcceptRole);
    }
    if (!r.notes.isEmpty()) box.setDetailedText(r.notes);
    QPushButton* skip = box.addButton(tr("&Skip This Version"), QMessageBox::DestructiveRole);
    box.addButton(tr("&Later"), QMessageBox::RejectRole);
    box.exec();

    if (box.clickedButton() == skip) {
        QSettings().setValue("updates/skip", r.version.toString());
        return;
    }
    if (page && box.clickedButton() == page) {
        QDesktopServices::openUrl(r.page);
        return;
    }
    if (!install_button || box.clickedButton() != install_button) return;

    auto* progress = new QProgressDialog(tr("Downloading Quillreel %1…").arg(r.version.toString()), tr("Cancel"), 0,
                                         1000, window_);
    progress->setWindowTitle(tr("Update"));
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setValue(0);
    busy_ = true;
    download(r, progress, [this, r, progress](std::expected<QString, QString> file) {
        busy_ = false;
        const bool cancelled = progress->wasCanceled();
        progress->deleteLater();
        if (cancelled) return;
        if (!file) {
            QMessageBox::warning(window_, tr("Update"), tr("The update failed: %1.").arg(file.error()));
            return;
        }
        if (auto ok = install(*file); !ok) {
            QMessageBox::warning(window_, tr("Update"), tr("The update failed: %1.").arg(ok.error()));
            return;
        }
        if (install_kind() == InstallKind::AppImage) {
            const auto again = QMessageBox::question(
                window_, tr("Update"),
                tr("Quillreel %1 is installed. Restart Quillreel now to use it?").arg(r.version.toString()));
            if (again != QMessageBox::Yes) return;
            QProcess::startDetached(QFileInfo(qEnvironmentVariable("APPIMAGE")).canonicalFilePath(), {});
        }
        // Windows: the installer is running and waits for Quillreel to close.
        window_->close();
        QApplication::quit();
    });
}

void Updater::download(const Release& r, QProgressDialog* progress,
                       std::function<void(std::expected<QString, QString>)> then) {
    // The AppImage is written next to the one it replaces (the same file system,
    // so the final rename is atomic); the installer goes to the temp folder.
    QString dest;
    if (install_kind() == InstallKind::AppImage) {
        const QFileInfo app(QFileInfo(qEnvironmentVariable("APPIMAGE")).canonicalFilePath());
        dest = app.absolutePath() + "/.quillreel-update-" + r.version.toString() + ".part";
    } else {
        dest = QDir::temp().filePath(r.asset);
    }
    auto file = std::make_shared<QFile>(dest);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate))
        return then(std::unexpected(QString("cannot write %1: %2").arg(QDir::toNativeSeparators(dest), file->errorString())));
    auto hash = std::make_shared<QCryptographicHash>(QCryptographicHash::Sha256);
    auto write_failed = std::make_shared<bool>(false);

    QNetworkRequest req(r.asset_url);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Quillreel/" + QApplication::applicationVersion());
    req.setTransferTimeout(60000);  // without any data
    QNetworkReply* reply = net_.get(req);
    connect(reply, &QNetworkReply::readyRead, this, [reply, file, hash, write_failed] {
        const QByteArray data = reply->readAll();
        hash->addData(data);
        if (file->write(data) != data.size()) {
            *write_failed = true;
            reply->abort();
        }
    });
    if (progress) {
        connect(reply, &QNetworkReply::downloadProgress, progress, [progress, size = r.size](qint64 got, qint64 total) {
            if (total <= 0) total = size;
            if (total > 0) progress->setValue(static_cast<int>(std::min<qint64>(got, total) * 1000 / total));
        });
        connect(progress, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
    }
    connect(reply, &QNetworkReply::finished, this, [reply, file, hash, write_failed, r, dest, then = std::move(then)] {
        reply->deleteLater();
        const QByteArray rest = reply->readAll();
        hash->addData(rest);
        if (file->write(rest) != rest.size()) *write_failed = true;
        file->close();
        auto failed = [&](const QString& why) {
            file->remove();
            then(std::unexpected(why));
        };
        if (*write_failed)
            return failed(QString("cannot write %1: %2").arg(QDir::toNativeSeparators(dest), file->errorString()));
        if (reply->error() == QNetworkReply::OperationCanceledError) return failed("cancelled");
        if (reply->error() != QNetworkReply::NoError) return failed("the download failed: " + reply->errorString());
        if (file->size() != r.size) return failed("the download is incomplete");
        if (hash->result() != r.sha256) return failed("the download doesn't match the checksum GitHub published for it");
        then(dest);
    });
}

std::expected<void, QString> Updater::install(const QString& file) {
    switch (install_kind()) {
    case InstallKind::AppImage: {
        namespace fs = std::filesystem;
        const fs::path target(QFile::encodeName(QFileInfo(qEnvironmentVariable("APPIMAGE")).canonicalFilePath()).toStdString());
        const fs::path tmp(QFile::encodeName(file).toStdString());
        std::error_code ec;
        // Same permissions as before, and executable. The running AppImage keeps
        // using the old file, which the rename only unlinks.
        const fs::perms perms = fs::status(target, ec).permissions();
        fs::permissions(tmp, (ec ? fs::perms::owner_all : perms) | fs::perms::owner_exec | fs::perms::group_exec |
                                 fs::perms::others_exec, ec);
        fs::rename(tmp, target, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return std::unexpected(QString("cannot replace %1: %2").arg(QString::fromStdString(target.string()),
                                                                         QString::fromStdString(ec.message())));
        }
        return {};
    }
    case InstallKind::WindowsInstaller: {
#ifdef _WIN32
        // /UPDATE: keep the components chosen at install time, wait for
        // Quillreel to close, and start it again afterwards (quillreel.nsi).
        const QString exe = QDir::toNativeSeparators(file);
        const auto h = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", reinterpret_cast<LPCWSTR>(exe.utf16()),
                                                               L"/S /UPDATE", nullptr, SW_SHOWNORMAL));
        if (h <= 32) return std::unexpected(QString("the installer didn't start (permission refused?)"));
        return {};
#else
        break;
#endif
    }
    case InstallKind::Notify:
        break;
    }
    return std::unexpected(QString("this copy of Quillreel can't update itself"));
}

void Updater::run_test(bool do_install, std::function<void(int)> done) {
    fetch([this, do_install, done](std::expected<Release, QString> r) {
        if (!r) {
            std::printf("update: error: %s\n", qPrintable(r.error()));
            std::fflush(stdout);
            return done(1);
        }
        const bool newer = r->version > current_version();
        std::printf("update: latest=%s current=%s newer=%d asset=%s\n", qPrintable(r->version.toString()),
                    qPrintable(current_version().toString()), newer ? 1 : 0, qPrintable(r->asset));
        std::fflush(stdout);
        if (!do_install || !newer || r->asset.isEmpty()) return done(0);
        download(*r, nullptr, [this, r = *r, done](std::expected<QString, QString> file) {
            std::expected<void, QString> ok = file ? install(*file) : std::unexpected(file.error());
            std::printf("update: %s\n", ok ? "installed" : qPrintable("error: " + ok.error()));
            std::fflush(stdout);
            done(ok ? 0 : 1);
        });
    });
}

}  // namespace quillreel::gui
