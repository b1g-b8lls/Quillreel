// SPDX-License-Identifier: GPL-3.0-or-later
// quillreel-gui: the Qt front end. Spec: specs/gui.md
#include <algorithm>
#include <QApplication>
#include <QCommandLineParser>
#include <QDialog>
#include <QFile>
#include <QIcon>
#include <QPalette>
#include <QStyle>
#include <QStyleFactory>
#include <QStyleHints>
#include <QStandardPaths>
#include <QKeySequence>
#include <QTimer>
#include <QTreeWidget>
#include <QPixmap>

#include <cstdio>

extern "C" {
#include <libavutil/log.h>
}

#include "bink2_dll.h"
#include "bink2_dialog.h"
#include "convert_dialog.h"
#include "main_window.h"
#include "player_widget.h"
#include "updater.h"

namespace {

// Dark mode: the desktop may ask for dark (the colour-scheme portal setting,
// GNOME's dark style) while the Qt theme in use only has light colours, e.g. the
// portal or GTK3 theme with Qt's Fusion style. Then use a dark palette. Themes
// that supply their own dark colours (KDE Plasma, a dark GTK theme) are left alone.
void follow_color_scheme(QApplication& app) {
    static bool overridden = false;
    if (overridden) {
        app.setPalette(QPalette());  // an empty palette hands colours back to the theme
        overridden = false;
    }
    const bool want_dark = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
    const bool theme_light = QApplication::palette().color(QPalette::Window).lightness() > 128;
    if (!want_dark || !theme_light) return;
    overridden = true;
    QPalette p;
    const QColor window(45, 45, 48), base(30, 30, 32), text(230, 230, 230), disabled(130, 130, 130);
    const QColor accent(52, 132, 228);
    p.setColor(QPalette::Window, window);
    p.setColor(QPalette::WindowText, text);
    p.setColor(QPalette::Base, base);
    p.setColor(QPalette::AlternateBase, window);
    p.setColor(QPalette::ToolTipBase, base);
    p.setColor(QPalette::ToolTipText, text);
    p.setColor(QPalette::PlaceholderText, disabled);
    p.setColor(QPalette::Text, text);
    p.setColor(QPalette::Button, window);
    p.setColor(QPalette::ButtonText, text);
    p.setColor(QPalette::BrightText, QColor(255, 90, 90));
    p.setColor(QPalette::Link, accent.lighter(130));
    p.setColor(QPalette::Highlight, accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Light, window.lighter(150));
    p.setColor(QPalette::Midlight, window.lighter(125));
    p.setColor(QPalette::Mid, window.darker(120));
    p.setColor(QPalette::Dark, window.darker(150));
    p.setColor(QPalette::Shadow, Qt::black);
    for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText, QPalette::HighlightedText})
        p.setColor(QPalette::Disabled, role, disabled);
    p.setColor(QPalette::Disabled, QPalette::Highlight, window.lighter(130));
    app.setPalette(p);
}

// The desktop switched colour scheme (e.g. Breeze to Breeze Dark) while running.
// Breeze reads the header colours it paints the menu bar with when the style is
// created, and a running plain-Qt app never sees them change, so the menu bar
// kept the old scheme's colour. A new style instance reads them again. Breeze
// takes them from KDE_COLOR_SCHEME_PATH if set; it is pointed at kdeglobals only
// while the style is made, spelled two ways in turn because KDE caches a config
// by name and the old style still holds the previous one. Deferred so the
// platform theme has updated the palette first; then re-check the dark palette.
void theme_changed(QApplication& app) {
    QTimer::singleShot(0, &app, [&app] {
        const QString name = app.style()->name();
        const QString globals =
            QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/kdeglobals";
        static bool alt = false;
        const bool breeze = name.compare("breeze", Qt::CaseInsensitive) == 0 && QFile::exists(globals);
        if (breeze) {
            alt = !alt;
            app.setProperty("KDE_COLOR_SCHEME_PATH", alt ? QString(globals).replace("/kdeglobals", "//kdeglobals")
                                                         : globals);
        }
        if (QStyle* style = QStyleFactory::create(name)) app.setStyle(style);
        if (breeze) app.setProperty("KDE_COLOR_SCHEME_PATH", QVariant());
        follow_color_scheme(app);
    });
}

}  // namespace

using quillreel::gui::ConvertDialog;
using quillreel::gui::MainWindow;

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("Quillreel");
    QApplication::setApplicationName("Quillreel");
    QApplication::setApplicationVersion("0.7.0");
    QApplication::setDesktopFileName("quillreel");
    av_log_set_level(AV_LOG_ERROR);
    // The installed theme icon if there is one, else the copy built into the program.
    QIcon builtin;
    for (int s : {16, 22, 24, 32, 48, 64, 128, 256}) builtin.addFile(QString(":/icons/quillreel-%1.png").arg(s));
    QApplication::setWindowIcon(QIcon::fromTheme("quillreel", builtin));
    follow_color_scheme(app);
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &app,
                     [&app] { theme_changed(app); });

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
    const QCommandLineOption test_convert =
        hidden({"test-convert",
                "Convert all listed files: EXT:DIR[,novideo][,audio=all|none|N][,overwrite][,quality=high|medium|small]"
                "[,crf=N][,speed=fast|slow][,height=N][,abitrate=K]",
                "spec"});
    const QCommandLineOption test_cancel = hidden({"test-convert-cancel-after", "Cancel after SECONDS.", "seconds"});
    const QCommandLineOption test_bink2 = hidden({"test-bink2", "After reading the list: install:PATH or remove.", "action"});
    const QCommandLineOption test_dialog = hidden({"test-dialog", "Open a dialog: convert or bink2.", "name"});
    const QCommandLineOption test_update =
        hidden({"test-update", "Check for an update (check), or also install it (install), then quit.", "action"});
    const QList<QCommandLineOption> test_options = {allow_non_bink, test_play,  test_quit,    test_keys,
                                                    test_list,      report,     test_silent,  test_shot,
                                                    test_convert,   test_cancel, test_bink2,  test_dialog,
                                                    test_update};
    parser.addOptions(test_options);
    parser.process(app);

    quillreel::gui::GuiOptions opts;
    opts.allow_non_bink = parser.isSet(allow_non_bink);
    MainWindow window(opts);
    window.show();

    // Test sequence, run each time the file list has been read:
    // --test-bink2 (then the list is read again), --test-convert, --test-dump-list.
    bool bink2_done = !parser.isSet(test_bink2), convert_started = false;
    auto after_scan = [&] {
        if (!bink2_done) {
            bink2_done = true;
            const QString action = parser.value(test_bink2);
            if (action.startsWith("install:")) {
                auto r = quillreel::bink2::install(action.mid(8).toStdString());
                std::printf("bink2: %s\n", r ? "installed" : r.error().message.c_str());
            } else {
                auto r = quillreel::bink2::uninstall();
                std::printf("bink2: %s\n", r ? (*r ? "removed" : "nothing to remove") : r.error().message.c_str());
            }
            std::fflush(stdout);
            window.refresh_bink2();  // reads the list again, then comes back here
            return;
        }
        if (parser.isSet(test_convert) && !convert_started) {
            convert_started = true;
            const QStringList parts = parser.value(test_convert).split(',');
            const int colon = parts[0].indexOf(':');
            window.select_all();
            ConvertDialog* d = window.make_convert_dialog();
            if (!d) {
                std::printf("convert: no files\n");
                app.exit(1);
                return;
            }
            d->set_format(parts[0].left(colon));
            d->set_output_dir(parts[0].mid(colon + 1));
            d->set_video(true);
            d->set_audio(0);
            for (const QString& p : parts.mid(1)) {
                if (p == "novideo") d->set_video(false);
                if (p == "overwrite") d->set_overwrite(true);
                if (p == "audio=all") d->set_audio(1);
                if (p == "audio=none") d->set_audio(3);
                if (p.startsWith("audio=") && p.mid(6).toInt() > 0) d->set_audio(2, p.mid(6).toInt());
                if (p == "audio=0") d->set_audio(2, 0);
                if (p == "quality=high") d->set_quality(1);
                if (p == "quality=medium") d->set_quality(2);
                if (p == "quality=small") d->set_quality(3);
                if (p.startsWith("crf=")) d->set_quality(4, p.mid(4).toInt());
                if (p == "speed=fast") d->set_speed(1);
                if (p == "speed=slow") d->set_speed(2);
                if (p.startsWith("height=")) d->set_height(p.mid(7).toInt());
                if (p.startsWith("abitrate=")) d->set_audio_bitrate(p.mid(9).toInt());
            }
            QObject::connect(d, &ConvertDialog::batch_finished, &app, [&app, d](int ok, int skipped, int failed) {
                for (const QString& line : d->log_text().split('\n')) std::printf("log: %s\n", qPrintable(line));
                std::printf("convert: converted=%d skipped=%d failed=%d\n", ok, skipped, failed);
                std::fflush(stdout);
                app.quit();
            });
            d->show();
            d->start();
            if (parser.isSet(test_cancel))
                QTimer::singleShot(static_cast<int>(parser.value(test_cancel).toDouble() * 1000), d,
                                   [d] { d->cancel(); });
            return;
        }
        if (parser.isSet(test_list)) {
            QTreeWidget* list = window.file_list();
            list->sortItems(0, Qt::AscendingOrder);
            for (int i = 0; i < list->topLevelItemCount(); ++i) {
                QStringList cols;
                for (int c = 0; c < list->columnCount(); ++c) cols << list->topLevelItem(i)->text(c);
                std::printf("row: %s\n", qPrintable(cols.join('\t')));
            }
            std::fflush(stdout);
            app.quit();
        }
    };
    QObject::connect(&window, &MainWindow::scan_finished, &app, after_scan, Qt::QueuedConnection);
    if (parser.isSet(test_silent)) window.player()->change_volume(-100);
    window.add_paths(parser.positionalArguments());
    if (parser.isSet(test_play)) window.play(parser.value(test_play));

    if (parser.isSet(test_keys)) {
        for (const QString& item : parser.value(test_keys).split(',', Qt::SkipEmptyParts)) {
            const int colon = item.indexOf(':');
            const double at = item.left(colon).toDouble();
            // "," separates the list, so the frame-step keys go by name.
            QString name = item.mid(colon + 1);
            if (name == "Comma") name = ",";
            if (name == "Period") name = ".";
            const QKeySequence keys(name);
            QTimer::singleShot(static_cast<int>(at * 1000), &window, [&window, keys] {
                if (!window.trigger_shortcut(keys)) std::fprintf(stderr, "no action for %s\n", qPrintable(keys.toString()));
            });
        }
    }
    if (parser.isSet(test_dialog)) {
        const QString name = parser.value(test_dialog);
        QTimer::singleShot(500, &window, [&window, name] {
            QDialog* d = nullptr;
            if (name == "bink2") {
                d = new quillreel::gui::Bink2Dialog(&window);
            } else {
                window.select_all();
                d = window.make_convert_dialog();
            }
            if (d) d->show();
        });
    }
    if (parser.isSet(test_shot)) {
        const QString spec = parser.value(test_shot);
        const int colon = spec.indexOf(':');
        const QString path = spec.mid(colon + 1);
        QTimer::singleShot(static_cast<int>(spec.left(colon).toDouble() * 1000), &window, [&window, path] {
            // The topmost open dialog if there is one, else the main window.
            QWidget* target = &window;
            for (QWidget* w : QApplication::topLevelWidgets())
                if (w != &window && w->isVisible() && qobject_cast<QDialog*>(w)) target = w;
            if (!target->grab().save(path)) std::fprintf(stderr, "cannot save %s\n", qPrintable(path));
        });
    }
    if (parser.isSet(test_update)) {
        window.updater()->run_test(parser.value(test_update) == "install", [&app](int rc) { app.exit(rc); });
    } else if (QGuiApplication::platformName() != "offscreen" &&
               std::ranges::none_of(test_options, [&](const QCommandLineOption& o) { return parser.isSet(o); })) {
        // Not in tests or headless runs: the first-start question would block them.
        window.updater()->start_automatic();
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
