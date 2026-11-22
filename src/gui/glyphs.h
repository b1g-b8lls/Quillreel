// SPDX-License-Identifier: GPL-3.0-or-later
// The transport bar's icons, drawn in the palette's colours so they stay
// readable on dark themes (the styles' built-in media icons are fixed dark
// pixmaps). Redraw them when the palette changes.
#pragma once

#include <QIcon>
#include <QPalette>

namespace quillreel::gui {

enum class Glyph { Play, Pause, Restart, Volume, Muted, Loop, Fullscreen };

// Normal: button text colour; Disabled: the disabled text colour; checked
// (QIcon::On): the highlight colour.
QIcon glyph_icon(Glyph g, const QPalette& pal);

}  // namespace quillreel::gui
