// SPDX-License-Identifier: GPL-3.0-or-later
// Small RAII and error helpers around FFmpeg's C API.
#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
}

namespace quillreel {

inline std::string av_error_string(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

// The codec tag is stored little-endian: "KB2g" == 'K' | 'B'<<8 | '2'<<16 | 'g'<<24.
inline std::string fourcc_string(uint32_t tag) {
    std::string s;
    for (int i = 0; i < 4; ++i) {
        char c = static_cast<char>((tag >> (8 * i)) & 0xff);
        s += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return s;
}

struct InputCloser {
    void operator()(AVFormatContext* c) const { avformat_close_input(&c); }
};
struct CodecCloser {
    void operator()(AVCodecContext* c) const { avcodec_free_context(&c); }
};
struct GraphCloser {
    void operator()(AVFilterGraph* g) const { avfilter_graph_free(&g); }
};
struct FrameCloser {
    void operator()(AVFrame* f) const { av_frame_free(&f); }
};
struct PacketCloser {
    void operator()(AVPacket* p) const { av_packet_free(&p); }
};

using InputPtr = std::unique_ptr<AVFormatContext, InputCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecCloser>;
using GraphPtr = std::unique_ptr<AVFilterGraph, GraphCloser>;
using FramePtr = std::unique_ptr<AVFrame, FrameCloser>;
using PacketPtr = std::unique_ptr<AVPacket, PacketCloser>;

}  // namespace quillreel
