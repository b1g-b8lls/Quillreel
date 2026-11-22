// SPDX-License-Identifier: GPL-3.0-or-later
// Playback core: demuxing, decoding and seeking on a worker thread. It has no
// UI or SDL dependency; a front end pulls video frames and supplies an AudioSink.
// Spec: specs/player.md
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "av_util.h"
#include "errors.h"

struct SwsContext;
struct SwrContext;

namespace quillreel {

namespace bink2 {
class Decoder;
}

struct PlayOptions {
    std::string input;
    bool video = true;
    bool audio = true;
    std::optional<int> audio_track;  // position in the inspect listing (0-based)
    double start = 0;                // seconds
    bool allow_non_bink = false;     // developer/testing: accept any FFmpeg-readable input
};

struct StreamSetup {
    bool has_video = false;
    int width = 0;
    int height = 0;
    AVRational sample_aspect{1, 1};
    double fps = 0;
    bool has_audio = false;
    int sample_rate = 0;  // of the selected track
    int channels = 0;
    int audio_tracks = 0;  // in the file
    int audio_track = -1;  // selected, -1 if none
    double duration = 0;   // seconds, 0 if unknown
    std::string video_note;  // why video is unavailable, if it is
    bool bink2_needs_dll = false;  // video unavailable only because no bink2w64.dll is provided
};

// Receives interleaved 32-bit float PCM. Methods are called from the worker
// thread and (buffered_seconds, clear) from the front end; must be thread-safe.
class AudioSink {
public:
    virtual ~AudioSink() = default;
    virtual bool configure(int sample_rate, int channels) = 0;
    virtual void write(const float* interleaved, int samples) = 0;
    virtual void clear() = 0;
    virtual void flush() = 0;  // end of stream: let any held-back samples play out
    virtual double buffered_seconds() const = 0;
};

struct VideoFrame {
    FramePtr frame;  // YUV420P (or YUVA420P; alpha is ignored by the front end)
    double pts = 0;  // seconds
};

class MediaSource {
public:
    static std::expected<std::unique_ptr<MediaSource>, Error> open(const PlayOptions& opts);
    ~MediaSource();
    MediaSource(const MediaSource&) = delete;
    MediaSource& operator=(const MediaSource&) = delete;

    const StreamSetup& setup() const { return setup_; }

    // Starts the worker. sink may be null: audio is then not decoded.
    void start(AudioSink* sink);

    // Asynchronous; the worker picks the request up between packets.
    void seek(double seconds);
    void select_audio_track(int track, double resume_at);

    // Front-end side of the video queue.
    bool peek_video(size_t index, double& pts) const;
    FramePtr pop_video();

    // Frame stepping (while paused). The source remembers the frames it handed
    // out most recently, and after a seek the ones decoded just before the
    // target (kHistoryFrames / kHistoryBytes). step_back() gives the frame
    // before the last one popped and puts that one back at the front of the
    // queue. False if it has none: seek to the frame instead, which in
    // single-keyframe files means decoding from the start.
    bool step_back(VideoFrame& out);
    // Lines the audio up with position t (the frame now shown, while paused):
    // drops audio before t, or refills from the last kAudioHistory seconds
    // played. Further back than that, it seeks.
    void align_audio(double t);

    double audio_clock() const;  // position of the sample being heard
    bool seeking() const;        // a seek is pending or has not produced output yet
    double seek_target() const;
    bool finished() const;  // input fully read and the video queue drained
    int audio_track() const;
    int rewinds() const { return rewinds_; }  // seeks that had to restart reading (for tests)

private:
    MediaSource() = default;

    struct Request {
        double target;
        int track;
    };

    void worker();
    void do_seek(const Request& r);
    bool open_audio_decoder(int track);
    void decode_video(const AVPacket* pkt);
    void decode_audio(const AVPacket* pkt);
    bool push_video(FramePtr frame, double pts);
    bool push_audio(const float* data, int samples, double start);
    void request_locked(Request r);  // mu_ must be held
    void remember_locked(VideoFrame v);   // adds to past_, mu_ must be held
    void record_audio_locked(const float* data, int samples);  // adds to audio_hist_
    FramePtr to_yuv420p(FramePtr frame);
    bool has_video_decoder() const { return vdec_ || b2dec_; }
    int video_send(const AVPacket* pkt);
    int video_receive(AVFrame* frame);
    void video_flush();

    // Fixed after open().
    InputPtr in_;
    AVStream* vst_ = nullptr;
    std::vector<AVStream*> audio_streams_;
    StreamSetup setup_;
    std::optional<Request> initial_;

    // Owned by the worker thread.
    CodecPtr vdec_;
    std::unique_ptr<bink2::Decoder> b2dec_;  // Bink 2 video through RAD's DLL (instead of vdec_)
    CodecPtr adec_;
    SwsContext* sws_ = nullptr;
    SwrContext* swr_ = nullptr;
    int sink_channels_ = 0;  // format the sink is configured for
    int sink_rate_ = 0;
    std::vector<float> pcm_;
    int64_t video_skip_until_ = AV_NOPTS_VALUE;  // in vst_ time base
    double video_drop_before_ = -1;
    double audio_drop_before_ = -1;
    double audio_next_pts_ = -1;  // position after the last decoded audio frame, -1 if unknown
    double read_pts_ = -1;        // furthest position read (selected streams) since the last rewind
    int64_t last_video_pkt_pts_ = AV_NOPTS_VALUE;
    bool worker_eof_ = false;
    std::atomic<int> rewinds_{0};
    int worker_track_ = -1;
    AudioSink* sink_ = nullptr;

    // Shared, guarded by mu_.
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<VideoFrame> vq_;
    std::deque<VideoFrame> past_;  // frames before vq_, oldest first; back() is the one shown
    size_t past_bytes_ = 0;
    std::deque<float> audio_hist_;  // interleaved samples just before audio_end_
    double audio_skip_to_ = -1;     // drop decoded audio before this position (after stepping ahead)
    std::optional<Request> pending_;
    bool seek_active_ = true;  // also covers start-up: the clock waits for the first output
    double seek_target_ = 0;
    bool eof_ = false;
    bool quit_ = false;
    double audio_end_ = 0;  // pts just past the last sample written to the sink
    bool audio_rebase_ = false;
    int track_ = -1;

    std::thread thread_;
};

}  // namespace quillreel
