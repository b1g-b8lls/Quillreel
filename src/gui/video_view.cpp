// SPDX-License-Identifier: GPL-3.0-or-later
#include "video_view.h"

#include <QMatrix3x3>
#include <QMouseEvent>

#include <algorithm>

namespace quillreel::gui {
namespace {

// GLSL that works on desktop OpenGL 2.x compatibility contexts and on GLES 2.
const char* kVertex = R"(
attribute vec2 pos;
attribute vec2 uv;
varying vec2 v_uv;
void main() {
    v_uv = uv;
    gl_Position = vec4(pos, 0.0, 1.0);
}
)";

const char* kFragment = R"(
#ifdef GL_ES
precision highp float;
#endif
uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;
uniform vec3 scale_u;   // per-plane horizontal texture coordinate scale
uniform mat3 yuv_to_rgb;
uniform vec3 offset;
varying vec2 v_uv;
void main() {
    vec3 yuv = vec3(texture2D(tex_y, vec2(v_uv.x * scale_u.x, v_uv.y)).r,
                    texture2D(tex_u, vec2(v_uv.x * scale_u.y, v_uv.y)).r,
                    texture2D(tex_v, vec2(v_uv.x * scale_u.z, v_uv.y)).r);
    gl_FragColor = vec4(yuv_to_rgb * (yuv - offset), 1.0);
}
)";

// Row-major Y'CbCr -> R'G'B' matrices and offsets, chosen like the SDL player:
// BT.601 limited range unless the frame says BT.709 and/or full range.
struct Matrix {
    float m[9];
    float off[3];
};
constexpr float kLimY = 255.0f / 219.0f, kLimC = 255.0f / 224.0f;
const Matrix kMatrices[4] = {
    // 0: BT.601 limited
    {{kLimY, 0, 1.402f * kLimC, kLimY, -0.344136f * kLimC, -0.714136f * kLimC, kLimY, 1.772f * kLimC, 0},
     {16.0f / 255, 0.5f, 0.5f}},
    // 1: BT.601 full
    {{1, 0, 1.402f, 1, -0.344136f, -0.714136f, 1, 1.772f, 0}, {0, 0.5f, 0.5f}},
    // 2: BT.709 limited
    {{kLimY, 0, 1.5748f * kLimC, kLimY, -0.187324f * kLimC, -0.468124f * kLimC, kLimY, 1.8556f * kLimC, 0},
     {16.0f / 255, 0.5f, 0.5f}},
    // 3: BT.709 full
    {{1, 0, 1.5748f, 1, -0.187324f, -0.468124f, 1, 1.8556f, 0}, {0, 0.5f, 0.5f}},
};

int matrix_for(const AVFrame* f) {
    const bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
    if (f->colorspace == AVCOL_SPC_BT709) return full ? 3 : 2;
    return full ? 1 : 0;
}

}  // namespace

VideoView::VideoView(QWidget* parent) : QOpenGLWidget(parent) {
    setMinimumSize(160, 90);
}

VideoView::~VideoView() {
    if (!gl_ok_) return;
    makeCurrent();
    glDeleteTextures(3, tex_);
    doneCurrent();
}

void VideoView::set_frame(FramePtr frame) {
    frame_ = std::move(frame);
    pending_ = true;
    update();
}

void VideoView::clear() {
    frame_.reset();
    pending_ = false;
    width_ = height_ = 0;
    update();
}

void VideoView::initializeGL() {
    initializeOpenGLFunctions();
    gl_ok_ = program_.addShaderFromSourceCode(QOpenGLShader::Vertex, kVertex) &&
             program_.addShaderFromSourceCode(QOpenGLShader::Fragment, kFragment);
    program_.bindAttributeLocation("pos", 0);
    program_.bindAttributeLocation("uv", 1);
    gl_ok_ = gl_ok_ && program_.link();
    if (!gl_ok_) {
        qWarning("quillreel: video shader failed: %s", qPrintable(program_.log()));
        return;
    }
    glGenTextures(3, tex_);
    for (GLuint t : tex_) {
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

void VideoView::upload() {
    const AVFrame* f = frame_.get();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int i = 0; i < 3; ++i) {
        const int w = i ? (f->width + 1) / 2 : f->width;
        const int h = i ? (f->height + 1) / 2 : f->height;
        // Upload whole padded rows (GLES 2 has no UNPACK_ROW_LENGTH) and crop
        // with the texture coordinate scale.
        const int tw = std::max(f->linesize[i], w);
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, tw, h, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[i]);
        u_scale_[i] = static_cast<float>(w) / static_cast<float>(tw);
    }
    width_ = f->width;
    height_ = f->height;
    matrix_ = matrix_for(f);
}

void VideoView::paintGL() {
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (!gl_ok_) return;
    if (pending_ && frame_) {
        upload();
        pending_ = false;
    }
    if (width_ <= 0 || height_ <= 0) return;

    // Letterbox: fit the display aspect ratio inside the widget.
    const double dpr = devicePixelRatioF();
    const double ow = width() * dpr, oh = height() * dpr;
    const double sar = sar_.num > 0 && sar_.den > 0 ? av_q2d(sar_) : 1.0;
    const double aspect = width_ * sar / height_;
    double dw = ow, dh = ow / aspect;
    if (dh > oh) {
        dh = oh;
        dw = oh * aspect;
    }
    const float sx = static_cast<float>(dw / ow), sy = static_cast<float>(dh / oh);
    // x, y, u, v; v runs top to bottom (row 0 of the frame is the top).
    const GLfloat quad[] = {-sx, -sy, 0, 1, sx, -sy, 1, 1, -sx, sy, 0, 0, sx, sy, 1, 0};

    program_.bind();
    for (int i = 0; i < 3; ++i) {
        glActiveTexture(GL_TEXTURE0 + i);
        glBindTexture(GL_TEXTURE_2D, tex_[i]);
    }
    program_.setUniformValue("tex_y", 0);
    program_.setUniformValue("tex_u", 1);
    program_.setUniformValue("tex_v", 2);
    program_.setUniformValue("scale_u", u_scale_[0], u_scale_[1], u_scale_[2]);
    const Matrix& m = kMatrices[matrix_];
    program_.setUniformValue("yuv_to_rgb", QMatrix3x3(m.m));
    program_.setUniformValue("offset", m.off[0], m.off[1], m.off[2]);
    program_.enableAttributeArray(0);
    program_.enableAttributeArray(1);
    program_.setAttributeArray(0, GL_FLOAT, quad, 2, 4 * sizeof(GLfloat));
    program_.setAttributeArray(1, GL_FLOAT, quad + 2, 2, 4 * sizeof(GLfloat));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    program_.disableAttributeArray(0);
    program_.disableAttributeArray(1);
    program_.release();
    glActiveTexture(GL_TEXTURE0);
}

void VideoView::mouseDoubleClickEvent(QMouseEvent* e) {
    emit double_clicked();
    e->accept();
}

}  // namespace quillreel::gui
