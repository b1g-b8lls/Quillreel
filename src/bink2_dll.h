// SPDX-License-Identifier: GPL-3.0-or-later
// Bink 2 video through RAD's own decoder, bink2w64.dll, which the user
// supplies from a game that ships it. Quillreel contains no Bink 2 video
// decoder of its own; without the DLL, .bk2 files play and convert audio only.
// Design notes: docs/bink2.md
#pragma once

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>

#include "av_util.h"
#include "errors.h"

namespace quillreel::bink2 {

// $XDG_DATA_HOME/quillreel/bink2w64.dll (default ~/.local/share/...).
std::string installed_path();
// The DLL that would be used: $QUILLREEL_BINK2_DLL if set, else the installed
// copy if it exists.
std::optional<std::string> dll_path();
// Tells the user how to provide the DLL.
std::string missing_message();

struct DllInfo {
    std::string path;
    std::string version;  // FileVersion resource, e.g. "1.992e"; empty if absent
};

// Loads the DLL (in this process) and checks it has everything Quillreel uses.
std::expected<DllInfo, Error> check(const std::string& path);
// check(), then copy to installed_path().
std::expected<DllInfo, Error> install(const std::string& path);
// Deletes the installed copy. Returns false if there was none.
std::expected<bool, Error> uninstall();
std::string file_version(const std::string& path);

// Decodes the video of one .bk2 file. Works like an FFmpeg decoder: each
// packet from FFmpeg's Bink demuxer (whose pts is the frame number) yields one
// YUV420P (or YUVA420P) frame. The DLL reads the file itself; the packet only
// says which frame is wanted. Use from one thread at a time.
class Decoder {
public:
    static std::expected<std::unique_ptr<Decoder>, Error> open(const std::string& path);
    ~Decoder();
    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    int width() const { return width_; }
    int height() const { return height_; }
    int64_t frames() const { return frames_; }
    AVPixelFormat pix_fmt() const { return alpha_ ? AV_PIX_FMT_YUVA420P : AV_PIX_FMT_YUV420P; }

    // Same return conventions as avcodec_send_packet / avcodec_receive_frame.
    // A null packet means end of stream.
    int send_packet(const AVPacket* pkt);
    int receive_frame(AVFrame* frame);
    void flush();

private:
    struct Impl;
    explicit Decoder(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
    int width_ = 0, height_ = 0;
    int64_t frames_ = 0;
    bool alpha_ = false;
};

}  // namespace quillreel::bink2
