// SPDX-License-Identifier: GPL-3.0-or-later
// File > Open in HandBrake: finds an installed HandBrake and opens Quillreel's
// lossless conversions in it. HandBrake can't read Bink itself, and Quillreel
// never bundles it. Spec: specs/gui.md, "Open in HandBrake".
#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace quillreel::gui {

struct HandBrake {
    QString program;
    QStringList args;          // before the source
    bool flatpak = false;      // fr.handbrake.ghb: files go through Flatpak's file forwarding
    bool takes_source = true;  // false on Windows: its GUI documents no way to be given a file
};

// The GTK program `ghb` on PATH, else the fr.handbrake.ghb Flatpak (Linux), or
// HandBrake.exe on PATH or in Program Files (Windows).
std::optional<HandBrake> find_handbrake();

// Starts HandBrake with the files (one file: that file; several: their folder,
// which HandBrake scans as a batch; the Flatpak gets the first file and the
// folder opens). Without takes_source it starts HandBrake and opens the folder.
// False with *error if it couldn't start.
bool open_in_handbrake(const HandBrake& hb, const QStringList& files, const QString& folder, QString* error);

}  // namespace quillreel::gui
