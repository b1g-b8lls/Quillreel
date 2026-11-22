// SPDX-License-Identifier: GPL-3.0-or-later
// "1 file" / "23 files". Qt only resolves "%n file(s)" plurals through a
// translation file, which Quillreel doesn't ship, so English plurals are
// picked here.
#pragma once

#include <QString>

namespace quillreel::gui {

inline QString count_of(qint64 n, const QString& one, const QString& many) {
    return QString("%1 %2").arg(n).arg(n == 1 ? one : many);
}

}  // namespace quillreel::gui
