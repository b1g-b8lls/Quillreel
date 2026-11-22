// SPDX-License-Identifier: GPL-3.0-or-later
#include "glyphs.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPolygonF>

namespace quillreel::gui {
namespace {

// Draws g on a 24x24 grid.
void draw(QPainter& p, Glyph g, const QColor& c) {
    p.setRenderHint(QPainter::Antialiasing);
    p.setBrush(c);
    p.setPen(Qt::NoPen);
    QPen line(c, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    auto speaker = [&] { p.drawPolygon(QPolygonF{{3.5, 9}, {7.5, 9}, {12.5, 4.5}, {12.5, 19.5}, {7.5, 15}, {3.5, 15}}); };
    switch (g) {
    case Glyph::Play:
        p.drawPolygon(QPolygonF{{7.5, 4.5}, {7.5, 19.5}, {19.5, 12}});
        break;
    case Glyph::Pause:
        p.drawRoundedRect(QRectF(6.5, 5, 4, 14), 1, 1);
        p.drawRoundedRect(QRectF(13.5, 5, 4, 14), 1, 1);
        break;
    case Glyph::Restart:
        p.drawRoundedRect(QRectF(5, 5.5, 2.5, 13), 1, 1);
        p.drawPolygon(QPolygonF{{19, 5.5}, {19, 18.5}, {8.5, 12}});
        break;
    case Glyph::Volume:
        speaker();
        p.setBrush(Qt::NoBrush);
        p.setPen(line);
        p.drawArc(QRectF(9, 8, 8, 8), -50 * 16, 100 * 16);
        p.drawArc(QRectF(7, 4.5, 14, 15), -50 * 16, 100 * 16);
        break;
    case Glyph::Muted:
        speaker();
        p.setPen(line);
        p.drawLine(QPointF(15.5, 9), QPointF(21, 15));
        p.drawLine(QPointF(21, 9), QPointF(15.5, 15));
        break;
    case Glyph::Loop: {
        // A circular arrow, open at the top right, the head pointing clockwise.
        p.setBrush(Qt::NoBrush);
        p.setPen(line);
        p.drawArc(QRectF(5, 5, 14, 14), 70 * 16, 280 * 16);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawPolygon(QPolygonF{{13.0, 2.0}, {18.5, 5.8}, {12.6, 9.0}});
        break;
    }
    case Glyph::Fullscreen:
        p.setBrush(Qt::NoBrush);
        p.setPen(line);
        for (const auto& [x, y, dx, dy] : {std::array{4.5, 4.5, 1.0, 1.0}, std::array{19.5, 4.5, -1.0, 1.0},
                                            std::array{4.5, 19.5, 1.0, -1.0}, std::array{19.5, 19.5, -1.0, -1.0}}) {
            p.drawLine(QPointF(x, y), QPointF(x + 5 * dx, y));
            p.drawLine(QPointF(x, y), QPointF(x, y + 5 * dy));
        }
        break;
    }
}

QPixmap render(Glyph g, const QColor& c, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.scale(size / 24.0, size / 24.0);
    draw(p, g, c);
    return pm;
}

}  // namespace

QIcon glyph_icon(Glyph g, const QPalette& pal) {
    QIcon icon;
    const QColor normal = pal.color(QPalette::Active, QPalette::ButtonText);
    const QColor disabled = pal.color(QPalette::Disabled, QPalette::ButtonText);
    const QColor on = pal.color(QPalette::Active, QPalette::Highlight);
    for (int size : {16, 20, 24, 32, 40, 48}) {
        icon.addPixmap(render(g, normal, size), QIcon::Normal, QIcon::Off);
        icon.addPixmap(render(g, disabled, size), QIcon::Disabled, QIcon::Off);
        icon.addPixmap(render(g, on, size), QIcon::Normal, QIcon::On);
        icon.addPixmap(render(g, disabled, size), QIcon::Disabled, QIcon::On);
    }
    return icon;
}

}  // namespace quillreel::gui
