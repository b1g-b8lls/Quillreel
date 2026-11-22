// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_details.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <format>

namespace quillreel::gui {
namespace {

QString tr(const char* s) { return QCoreApplication::translate("FileDetails", s); }

QString channels_text(int n) {
    switch (n) {
    case 1: return tr("mono");
    case 2: return tr("stereo");
    case 6: return tr("5.1 surround");
    case 8: return tr("7.1 surround");
    default: return tr("%1 channels").arg(n);
    }
}

QString khz(int rate) { return QString::number(rate / 1000.0, 'g', 4) + " kHz"; }

// The file name as a heading, its folder under it.
QString heading(const QString& path, const QPalette& pal) {
    const QFileInfo fi(path);
    return QString("<p style='margin:0'><b style='font-size:large'>%1</b><br>"
                   "<span style='color:%2'>%3</span></p>")
        .arg(fi.fileName().toHtmlEscaped(), pal.color(QPalette::PlaceholderText).name(),
             QDir::toNativeSeparators(fi.absolutePath()).toHtmlEscaped());
}

QString row(const QString& label, const QString& value, const QPalette& pal) {
    return QString("<tr><td style='color:%1; padding-right:16px'>%2</td><td>%3</td></tr>")
        .arg(pal.color(QPalette::PlaceholderText).name(), label.toHtmlEscaped(), value);
}

}  // namespace

QString format_length(double s) {
    auto t = static_cast<long long>(s + 0.5);
    return t >= 3600 ? QString::fromStdString(std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60))
                     : QString::fromStdString(std::format("{}:{:02}", t / 60, t % 60));
}

QString format_size(int64_t bytes) {
    if (bytes >= (int64_t{1} << 30)) return QString::number(static_cast<double>(bytes) / (1 << 30), 'f', 2) + " GB";
    if (bytes >= (1 << 20)) return QString::number(static_cast<double>(bytes) / (1 << 20), 'f', 1) + " MB";
    return QString::number(static_cast<double>(bytes) / 1024, 'f', 0) + " KB";
}

QString details_html(const FileInfo& f, const QPalette& pal) {
    QString rows;
    if (f.video) {
        const VideoInfo& v = *f.video;
        rows += row(tr("Type"), (v.generation == "unknown" ? tr("Bink video") : tr("%1 video").arg(QString::fromStdString(v.generation))).toHtmlEscaped(), pal);
        rows += row(tr("Length"), f.duration_s > 0 ? format_length(f.duration_s) : tr("unknown"), pal);
        const double fps = v.frame_rate.den ? static_cast<double>(v.frame_rate.num) / v.frame_rate.den : 0;
        rows += row(tr("Picture"),
                    tr("%1 × %2 at %3 frames per second").arg(v.width).arg(v.height).arg(fps, 0, 'g', 4), pal);
        QString video;
        if (!v.decoder.empty())
            video = v.decoder == "bink2w64.dll" ? tr("Plays and converts, using RAD's bink2w64.dll")
                                                : tr("Plays and converts");
        else if (v.generation == "Bink 2")
            video = tr("<b>Not enabled</b>: Bink 2 video needs RAD's bink2w64.dll (Help &gt; Bink 2 Video). "
                       "The sound plays and converts without it.");
        else
            video = tr("Can't be shown: no decoder for it");
        rows += row(tr("Video"), video, pal);
    } else {
        rows += row(tr("Type"), tr("Bink audio"), pal);
        rows += row(tr("Length"), f.duration_s > 0 ? format_length(f.duration_s) : tr("unknown"), pal);
    }

    QString sound;
    if (f.audio.empty()) {
        sound = tr("None");
    } else {
        // "2 tracks, each stereo at 48 kHz" when they're alike, else one line per track.
        const AudioTrack& a0 = f.audio.front();
        bool alike = true;
        for (const AudioTrack& a : f.audio)
            alike = alike && a.channels == a0.channels && a.sample_rate == a0.sample_rate && a.decodable;
        if (alike) {
            sound = f.audio.size() == 1
                        ? tr("%1 at %2").arg(channels_text(a0.channels), khz(a0.sample_rate))
                        : tr("%1 tracks, each %2 at %3").arg(f.audio.size()).arg(channels_text(a0.channels), khz(a0.sample_rate));
        } else {
            QStringList lines;
            for (size_t i = 0; i < f.audio.size(); ++i) {
                const AudioTrack& a = f.audio[i];
                QString l = tr("Track %1: %2 at %3").arg(i).arg(channels_text(a.channels), khz(a.sample_rate));
                if (!a.decodable) l += tr(" (can't be played)");
                lines << l;
            }
            sound = lines.join("<br>");
        }
    }
    rows += row(tr("Sound"), sound, pal);
    rows += row(tr("Size"), format_size(f.file_size), pal);
    return heading(QString::fromStdString(f.path), pal) + "<table style='margin-top:6px'>" + rows + "</table>";
}

QString details_error_html(const QString& path, const QString& message, const QPalette& pal) {
    return heading(path, pal) + QString("<p>%1</p>").arg(tr("Quillreel can't read this file: %1").arg(message.toHtmlEscaped()));
}

QString details_reading_html(const QString& path, const QPalette& pal) {
    return heading(path, pal) + QString("<p>%1</p>").arg(tr("Reading…"));
}

}  // namespace quillreel::gui
