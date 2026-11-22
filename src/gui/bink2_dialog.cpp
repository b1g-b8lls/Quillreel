// SPDX-License-Identifier: GPL-3.0-or-later
#include "bink2_dialog.h"

#include <QApplication>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <cstdlib>
#include <filesystem>

#include "bink2_dll.h"

namespace quillreel::gui {

Bink2Dialog::Bink2Dialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Bink 2 Video"));
    status_ = new QLabel(this);
    status_->setWordWrap(true);
    status_->setTextFormat(Qt::RichText);
    status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    status_->setMinimumWidth(460);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    install_ = buttons->addButton(tr("&Install DLL…"), QDialogButtonBox::ActionRole);
    remove_ = buttons->addButton(tr("&Remove"), QDialogButtonBox::ActionRole);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(install_, &QPushButton::clicked, this, &Bink2Dialog::install);
    connect(remove_, &QPushButton::clicked, this, &Bink2Dialog::remove);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(status_);
    layout->addStretch();
    layout->addWidget(buttons);
    refresh();
}

void Bink2Dialog::refresh() {
    const QString installed = QString::fromStdString(bink2::installed_path());
    const bool has_installed = std::filesystem::is_regular_file(bink2::installed_path());
    const char* env = std::getenv("QUILLREEL_BINK2_DLL");
    QString text;
    if (auto path = bink2::dll_path()) {
        const QString version = QString::fromStdString(bink2::file_version(*path));
        text = tr("<p><b>Bink 2 video is enabled.</b> .bk2 video can be played and converted.</p>"
                  "<p>Decoder: %1<br>Version: %2</p>")
                   .arg(QString::fromStdString(*path).toHtmlEscaped(), version.isEmpty() ? tr("unknown") : version);
        if (env && *env)
            text += tr("<p><i>Set by the QUILLREEL_BINK2_DLL environment variable, which overrides the "
                       "installed copy.</i></p>");
    } else {
        text = tr("<p><b>Bink 2 video is not enabled.</b></p>"
                  "<p>Quillreel does not include a Bink 2 video decoder. Until you provide RAD's "
                  "<b>bink2w64.dll</b>, .bk2 files can be inspected and their audio played and converted, "
                  "but not their video.</p>"
                  "<p>Click <b>Install DLL…</b> and pick <tt>bink2w64.dll</tt> from the folder of a game "
                  "that uses Bink 2 (for example Fallout 4 or GTA V). It must be the 64-bit "
                  "<tt>bink2w64.dll</tt>; <tt>bink2w32.dll</tt> won't work. Quillreel checks it and keeps "
                  "a copy in:<br><tt>%1</tt></p>")
                   .arg(installed.toHtmlEscaped());
    }
    status_->setText(text);
    remove_->setEnabled(has_installed);
    install_->setText(has_installed ? tr("&Replace DLL…") : tr("&Install DLL…"));
}

void Bink2Dialog::install() {
    QSettings settings;
    const QString start = settings.value("bink2/last_dir", QDir::homePath()).toString();
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose bink2w64.dll"), start,
        tr("Bink 2 decoder (bink2w64.dll BINK2W64.DLL Bink2w64.dll);;DLL files (*.dll *.DLL);;All files (*)"));
    if (path.isEmpty()) return;
    settings.setValue("bink2/last_dir", QFileInfo(path).absolutePath());
    QApplication::setOverrideCursor(Qt::WaitCursor);
    auto r = bink2::install(path.toStdString());
    QApplication::restoreOverrideCursor();
    if (!r) {
        QMessageBox::warning(this, tr("Bink 2 Video"),
                             tr("That DLL can't be used:\n\n%1").arg(QString::fromStdString(r.error().message)));
        return;
    }
    refresh();
    emit changed();
    QMessageBox::information(this, tr("Bink 2 Video"),
                             tr("Bink 2 video is enabled (version %1).")
                                 .arg(r->version.empty() ? tr("unknown") : QString::fromStdString(r->version)));
}

void Bink2Dialog::remove() {
    if (QMessageBox::question(this, tr("Bink 2 Video"),
                              tr("Remove the installed bink2w64.dll? Bink 2 video will be disabled again.")) !=
        QMessageBox::Yes)
        return;
    auto r = bink2::uninstall();
    if (!r) QMessageBox::warning(this, tr("Bink 2 Video"), QString::fromStdString(r.error().message));
    refresh();
    emit changed();
}

}  // namespace quillreel::gui
