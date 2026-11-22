// SPDX-License-Identifier: GPL-3.0-or-later
// Shows YUV420P frames, letterboxed, converting to RGB on the GPU with the same
// colour matrix choice as the SDL player. Spec: specs/gui.md
#pragma once

#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>

#include "av_util.h"

namespace quillreel::gui {

class VideoView final : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit VideoView(QWidget* parent = nullptr);
    ~VideoView() override;

    void set_frame(FramePtr frame);  // takes the frame; repaints
    void clear();                    // back to black
    void set_sample_aspect(AVRational sar) { sar_ = sar; }

signals:
    void double_clicked();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;

private:
    void upload();

    QOpenGLShaderProgram program_;
    GLuint tex_[3] = {0, 0, 0};
    bool gl_ok_ = false;
    FramePtr frame_;      // latest frame, uploaded on the next paint
    bool pending_ = false;
    int width_ = 0, height_ = 0;  // of the uploaded frame
    float u_scale_[3] = {1, 1, 1};  // visible width / texture width (rows are padded)
    int matrix_ = 0;                // index into the colour matrices
    AVRational sar_{1, 1};
};

}  // namespace quillreel::gui
