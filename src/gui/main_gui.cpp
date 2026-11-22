// SPDX-License-Identifier: GPL-3.0-or-later
// quillreel-gui: the Qt front end. Spec: specs/gui.md
#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QKeySequence>
#include <QTimer>
#include <QTreeWidget>
#include <QPixmap>

#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

#include "main_window.h"
#include "player_widget.h"

using quillreel::gui::MainWindow;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Quillreel");
    QApplication::setApplicationName("Quillreel");
    QApplication::setApplicationVersion("0.4.0");
    QApplication::setDesktopFileName("quillreel");
    av_log_set_level(AV_LOG_ERROR);
    // The installed theme icon if there is one, else the copy built into the program.
    QIcon builtin;
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256}) builtin.addFile(QString(":/icons/quillreel-%1.png").arg(s));
    QApplication::setWindowIcon(QIcon::fromTheme("quillreel", builtin));

    QCommandLineParser parser;
    parser.setApplicationDescription("Inspect and play Bink video files (.bik, .bk2).");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("paths", "Bink files or folders to list.", "[FILE|FOLDER...]");
    auto hidden = [](QCommandLineOption o) {
        o.setFlags(QCommandLineOption::HiddenFromHelp);
        return o;
    };
    // Testing hooks, not shown in --help.
    const QCommandLineOption allow_non_bink = hidden({"allow-non-bink", "Accept any FFmpeg-readable input."});
    const QCommandLineOption test_play = hidden({"test-play", "Play FILE at start.", "file"});
    const QCommandLineOption test_quit = hidden({"test-quit-after", "Quit after SECONDS.", "seconds"});
    const QCommandLineOption test_keys = hidden({"test-keys", "Shortcuts to trigger: 1.0:Right,2.5:Space", "list"});
    const QCommandLineOption test_list = hidden({"test-dump-list", "Print the file list once read, then quit."});
    const QCommandLineOption report = hidden({"report", "Print playback statistics at exit."});
    const QCommandLineOption test_silent = hidden({"test-silent", "Play at volume 0."});
    const QCommandLineOption test_shot = hidden({"test-screenshot", "Save the window as PNG: SECONDS:PATH", "spec"});
    parser.addOptions({allow_non_bink, test_play, test_quit, test_keys, test_list, report, test_silent, test_shot});
    parser.process(app);

    quillreel::gui::GuiOptions opts;
    opts.allow_non_bink = parser.isSet(allow_non_bink);
    MainWindow window(opts);
    window.show();

    if (parser.isSet(test_list)) {
        QObject::connect(&window, &MainWindow::scan_finished, &app, [&] {
            QTreeWidget* list = window.file_list();
            list->sortItems(0, Qt::AscendingOrder);
            for (int i = 0; i < list->topLevelItemCount(); ++i) {
                QStringList cols;
                for (int c = 0; c < list->columnCount(); ++c) cols << list->topLevelItem(i)->text(c);
                std::printf("row: %s\n", qPrintable(cols.join('\t')));
            }
            std::fflush(stdout);
            app.quit();
        });
    }
    if (parser.isSet(test_silent)) window.player()->change_volume(-100);
    window.add_paths(parser.positionalArguments());
    if (parser.isSet(test_play)) window.play(parser.value(test_play));

    if (parser.isSet(test_keys)) {
        for (const QString& item : parser.value(test_keys).split(',', Qt::SkipEmptyParts)) {
            const int colon = item.indexOf(':');
            const double at = item.left(colon).toDouble();
            const QKeySequence keys(item.mid(colon + 1));
            QTimer::singleShot(static_cast<int>(at * 1000), &window, [&window, keys] {
                if (!window.trigger_shortcut(keys)) std::fprintf(stderr, "no action for %s\n", qPrintable(keys.toString()));
            });
        }
    }
    if (parser.isSet(test_shot)) {
        const QString spec = parser.value(test_shot);
        const int colon = spec.indexOf(':');
        const QString path = spec.mid(colon + 1);
        QTimer::singleShot(static_cast<int>(spec.left(colon).toDouble() * 1000), &window, [&window, path] {
            if (!window.grab().save(path)) std::fprintf(stderr, "cannot save %s\n", qPrintable(path));
        });
    }
    if (parser.isSet(test_quit))
        QTimer::singleShot(static_cast<int>(parser.value(test_quit).toDouble() * 1000), &window, &QWidget::close);
    if (parser.isSet(test_play))  // a finished --test-play ends the run (--test-quit-after is a time limit)
        QObject::connect(window.player(), &quillreel::gui::PlayerWidget::playback_finished, &window, &QWidget::close);

    int rc = app.exec();
    if (parser.isSet(report)) {
        const quillreel::gui::PlaybackStats s = window.player()->stats();
        std::printf("report: shown=%lld dropped=%lld first_pts=%.3f last_pts=%.3f audio_clock=%.3f track=%d rewinds=%d\n",
                    s.shown, s.dropped, s.first_pts, s.last_pts, s.audio_clock, s.track, s.rewinds);
    }
    return rc;
}
