// SPDX-License-Identifier: GPL-3.0-or-later
#include "handbrake.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>

namespace quillreel::gui {

std::optional<HandBrake> find_handbrake() {
#ifdef _WIN32
    QStringList candidates;
    if (const QString onpath = QStandardPaths::findExecutable("HandBrake"); !onpath.isEmpty()) candidates << onpath;
    for (const char* var : {"ProgramFiles", "ProgramW6432", "ProgramFiles(x86)", "LOCALAPPDATA"}) {
        const QString base = qEnvironmentVariable(var);
        if (!base.isEmpty()) candidates << QDir(base).filePath("HandBrake/HandBrake.exe");
    }
    for (const QString& c : candidates)
        if (QFileInfo(c).isExecutable()) return HandBrake{c, {}, false, false};
    return std::nullopt;
#else
    // HandBrake's GTK program; `-d` loads a file or folder on startup.
    if (const QString ghb = QStandardPaths::findExecutable("ghb"); !ghb.isEmpty()) return HandBrake{ghb, {"-d"}, false, true};
    if (const QString flatpak = QStandardPaths::findExecutable("flatpak"); !flatpak.isEmpty()) {
        QProcess p;
        p.start(flatpak, {"info", "fr.handbrake.ghb"});
        if (p.waitForFinished(5000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0)
            return HandBrake{flatpak, {"run", "--file-forwarding", "fr.handbrake.ghb", "-d"}, true, true};
    }
    return std::nullopt;
#endif
}

bool open_in_handbrake(const HandBrake& hb, const QStringList& files, const QString& folder, QString* error) {
    if (!hb.takes_source) {
        const bool ok = QProcess::startDetached(hb.program, {});
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        if (!ok && error) *error = QString("couldn't start %1").arg(QDir::toNativeSeparators(hb.program));
        return ok;
    }
    QString source = files.size() == 1 ? files.front() : folder;
    QStringList args = hb.args;
    if (hb.flatpak) {
        // File forwarding grants the sandbox exactly this file; it can't pass a
        // folder, so with several files HandBrake gets the first and the folder opens.
        if (files.size() > 1) {
            source = files.front();
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        }
        args << "@@" << source << "@@";
    } else {
        args << source;
    }
    const bool ok = QProcess::startDetached(hb.program, args);
    if (!ok && error) *error = QString("couldn't start %1").arg(hb.program);
    return ok;
}

}  // namespace quillreel::gui
