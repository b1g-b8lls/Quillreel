// SPDX-License-Identifier: GPL-3.0-or-later
// The file details panel's summary: what a file is, in plain words (View >
// Technical Details shows the `quillreel inspect` report instead).
// Spec: specs/gui.md, "Window".
#pragma once

#include <QPalette>
#include <QString>
#include <cstdint>

#include "inspect.h"

namespace quillreel::gui {

QString format_length(double seconds);  // 0:42, 1:02:03
QString format_size(int64_t bytes);     // 26.1 MB

// HTML for a QTextBrowser, in the palette's colours.
QString details_html(const FileInfo& f, const QPalette& pal);
QString details_error_html(const QString& path, const QString& message, const QPalette& pal);
QString details_reading_html(const QString& path, const QPalette& pal);

}  // namespace quillreel::gui
