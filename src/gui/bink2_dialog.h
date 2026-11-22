// SPDX-License-Identifier: GPL-3.0-or-later
// Help > Bink 2 Video: shows whether Bink 2 video is enabled, and installs or
// removes the user's bink2w64.dll (the same as `quillreel bink2`). Spec: specs/gui.md
#pragma once

#include <QDialog>

class QLabel;
class QPushButton;

namespace quillreel::gui {

class Bink2Dialog final : public QDialog {
    Q_OBJECT
public:
    explicit Bink2Dialog(QWidget* parent = nullptr);

signals:
    void changed();  // a DLL was installed or removed

private:
    void refresh();
    void install();
    void remove();

    QLabel* status_ = nullptr;
    QPushButton* install_ = nullptr;
    QPushButton* remove_ = nullptr;
};

}  // namespace quillreel::gui
