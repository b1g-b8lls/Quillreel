// SPDX-License-Identifier: GPL-3.0-or-later
#include "player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string_view>

#include "bink2_dll.h"

extern "C" {
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace quillreel {
namespace {

constexpr size_t kVideoQueueFrames = 6;
// Bink files store audio about 0.76 s ahead of the matching video, so the audio
// buffer must hold more than that or the worker could wait on itself.
constexpr double kMaxAudioBuffer = 1.5;

std::unexpected<Error> fail(ErrorKind kind, std::string message) {
    return std::unexpected(Error{kind, std::move(message)});
}

CodecPtr open_decoder(const AVStream* st) {
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) return nullptr;
    CodecPtr ctx(avcodec_alloc_context3(codec));
    if (!ctx || avcodec_parameters_to_context(ctx.get(), st->codecpar) < 0) return nullptr;
    ctx->pkt_timebase = st->time_base;
    if (avcodec_open2(ctx.get(), codec, nullptr) < 0) return nullptr;
    return ctx;
}

}  // namespace

std::expected<std::unique_ptr<MediaSource>, Error> MediaSource::open(const PlayOptions& opts) {
    std::unique_ptr<MediaSource> src(new MediaSource);
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, opts.input.c_str(), nullptr, nullptr);
    if (ret < 0) return fail(ErrorKind::Open, std::format("{}: {}", opts.input, av_error_string(ret)));
    src->in_.reset(raw);
    AVFormatContext* in = src->in_.get();

    if (std::string_view(in->iformat->name) != "bink") {
        if (!opts.allow_non_bink)
            return fail(ErrorKind::NotBink,
                        std::format("{}: not a Bink file (detected format: {})", opts.input, in->iformat->name));
        ret = avformat_find_stream_info(in, nullptr);
        if (ret < 0) return fail(ErrorKind::Open, std::format("{}: {}", opts.input, av_error_string(ret)));
    }

    AVStream* video = nullptr;
    for (unsigned i = 0; i < in->nb_streams; ++i) {
        AVStream* st = in->streams[i];
        if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
            if (!video) video = st;
        } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            src->audio_streams_.push_back(st);
        }
    }

    StreamSetup& s = src->setup_;
    s.audio_tracks = static_cast<int>(src->audio_streams_.size());
    if (video && video->duration != AV_NOPTS_VALUE)
        s.duration = static_cast<double>(video->duration) * av_q2d(video->time_base);
    else if (in->duration != AV_NOPTS_VALUE)
        s.duration = static_cast<double>(in->duration) / AV_TIME_BASE;

    if (opts.video && video) {
        std::string tag = fourcc_string(video->codecpar->codec_tag);
        if (tag.starts_with("KB2")) {
            // Bink 2 video only through RAD's own decoder, supplied by the user.
            if (!bink2::dll_path()) {
                s.video_note = "Bink 2 video needs bink2w64.dll (see: quillreel bink2); playing audio only";
            } else if (auto d = bink2::Decoder::open(opts.input)) {
                src->b2dec_ = std::move(*d);
            } else {
                s.video_note = d.error().message + "; playing audio only";
            }
        } else {
            src->vdec_ = open_decoder(video);
            if (!src->vdec_) s.video_note = std::format("cannot decode this video ({}); playing audio only", tag);
        }
        if (src->has_video_decoder()) {
            src->vst_ = video;
            s.has_video = true;
            s.width = src->b2dec_ ? src->b2dec_->width() : src->vdec_->width;
            s.height = src->b2dec_ ? src->b2dec_->height() : src->vdec_->height;
            AVRational sar = av_guess_sample_aspect_ratio(in, video, nullptr);
            if (sar.num > 0 && sar.den > 0) s.sample_aspect = sar;
            s.fps = av_q2d(stream_frame_rate(in, video));
        }
    }

    if (opts.audio_track && src->audio_streams_.empty())
        return fail(ErrorKind::Usage, "the input has no audio tracks");
    if (opts.audio && !src->audio_streams_.empty()) {
        int t = opts.audio_track.value_or(0);
        if (t < 0 || t >= s.audio_tracks)
            return fail(ErrorKind::Usage, std::format("audio track {} does not exist (the input has {}: 0-{})", t,
                                                      s.audio_tracks, s.audio_tracks - 1));
        if (src->open_audio_decoder(t)) {
            const AVCodecParameters* par = src->audio_streams_[t]->codecpar;
            s.has_audio = true;
            s.audio_track = t;
            s.sample_rate = par->sample_rate > 0 ? par->sample_rate : 48000;
            s.channels = par->ch_layout.nb_channels > 0 ? par->ch_layout.nb_channels : 2;
            src->track_ = t;
        } else if (opts.audio_track) {
            return fail(ErrorKind::Unsupported, std::format("cannot decode audio track {}", t));
        }
    }

    if (!s.has_video && !s.has_audio)
        return fail(ErrorKind::Unsupported,
                    s.video_note.empty() ? "nothing playable in this file with these options"
                                         : "nothing playable: " + s.video_note.substr(0, s.video_note.find(';')));
    if (opts.start > 0) src->initial_ = Request{opts.start, src->track_};
    return src;
}

MediaSource::~MediaSource() {
    {
        std::lock_guard lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    sws_freeContext(sws_);
    swr_free(&swr_);
}

void MediaSource::start(AudioSink* sink) {
    sink_ = sink;
    if (!sink_) {
        adec_.reset();
        worker_track_ = -1;
    } else {
        sink_channels_ = setup_.channels;
        sink_rate_ = setup_.sample_rate;
    }
    if (initial_) {
        std::lock_guard lk(mu_);
        request_locked(*initial_);
    }
    thread_ = std::thread(&MediaSource::worker, this);
}

void MediaSource::request_locked(Request r) {
    r.target = std::max(0.0, setup_.duration > 0 ? std::min(r.target, setup_.duration) : r.target);
    pending_ = r;
    track_ = r.track;
    seek_active_ = true;
    seek_target_ = r.target;
    eof_ = false;
    vq_.clear();
    if (sink_) sink_->clear();
    audio_end_ = r.target;
    audio_rebase_ = true;
    cv_.notify_all();
}

void MediaSource::seek(double seconds) {
    std::lock_guard lk(mu_);
    request_locked({seconds, track_});
}

void MediaSource::select_audio_track(int track, double resume_at) {
    std::lock_guard lk(mu_);
    if (track < 0 || track >= static_cast<int>(audio_streams_.size()) || !sink_) return;
    request_locked({resume_at, track});
}

bool MediaSource::peek_video(size_t index, double& pts) const {
    std::lock_guard lk(mu_);
    if (index >= vq_.size()) return false;
    pts = vq_[index].pts;
    return true;
}

FramePtr MediaSource::pop_video() {
    std::lock_guard lk(mu_);
    if (vq_.empty()) return nullptr;
    FramePtr f = std::move(vq_.front().frame);
    vq_.pop_front();
    cv_.notify_all();
    return f;
}

double MediaSource::audio_clock() const {
    std::lock_guard lk(mu_);
    return sink_ ? audio_end_ - sink_->buffered_seconds() : 0;
}

bool MediaSource::seeking() const {
    std::lock_guard lk(mu_);
    return seek_active_;
}

double MediaSource::seek_target() const {
    std::lock_guard lk(mu_);
    return seek_target_;
}

bool MediaSource::finished() const {
    std::lock_guard lk(mu_);
    return eof_ && vq_.empty() && !pending_;
}

int MediaSource::audio_track() const {
    std::lock_guard lk(mu_);
    return track_;
}

bool MediaSource::open_audio_decoder(int track) {
    adec_.reset();
    worker_track_ = -1;
    swr_free(&swr_);
    if (track < 0 || track >= static_cast<int>(audio_streams_.size())) return false;
    adec_ = open_decoder(audio_streams_[track]);
    if (!adec_) return false;
    worker_track_ = track;
    return true;
}

void MediaSource::worker() {
    PacketPtr pkt(av_packet_alloc());
    if (!pkt) return;
    while (true) {
        std::optional<Request> req;
        {
            std::unique_lock lk(mu_);
            cv_.wait(lk, [&] { return quit_ || pending_ || !eof_; });
            if (quit_) return;
            if (pending_) {
                req = pending_;
                pending_.reset();
            }
        }
        if (req) {
            do_seek(*req);
            continue;
        }

        int ret = av_read_frame(in_.get(), pkt.get());
        if (ret == AVERROR(EAGAIN)) continue;
        if (ret < 0) {  // end of input; a read error ends playback the same way
            worker_eof_ = true;
            if (has_video_decoder()) decode_video(nullptr);
            if (adec_) decode_audio(nullptr);
            std::lock_guard lk(mu_);
            if (!pending_) {
                eof_ = true;
                seek_active_ = false;
                if (sink_) sink_->flush();
            }
            continue;
        }
        const AVStream* st = in_->streams[pkt->stream_index];
        bool video = has_video_decoder() && pkt->stream_index == vst_->index;
        bool audio = !video && adec_ && pkt->stream_index == audio_streams_[worker_track_]->index;
        if ((video || audio) && pkt->pts != AV_NOPTS_VALUE) {
            read_pts_ = std::max(read_pts_, static_cast<double>(pkt->pts + std::max<int64_t>(pkt->duration, 0)) *
                                                av_q2d(st->time_base));
            if (video) last_video_pkt_pts_ = pkt->pts;
        }
        if (video)
            decode_video(pkt.get());
        else if (audio)
            decode_audio(pkt.get());
        av_packet_unref(pkt.get());
    }
}

// FFmpeg's Bink demuxer can only seek back to the start of the file, and the
// files may have a single keyframe. So a forward seek just keeps decoding from
// where the worker is, if nothing at the target has been read yet and no
// keyframe lies in between. Otherwise: seek as close as the demuxer allows,
// skip video packets (undecoded) up to the last indexed keyframe before the
// target, then decode. Either way, frames and samples before the target are dropped.
void MediaSource::do_seek(const Request& r) {
    const int64_t ts = std::llround(r.target * AV_TIME_BASE);
    int64_t keyframe = AV_NOPTS_VALUE;  // last indexed keyframe at or before the target
    if (has_video_decoder()) {
        int64_t vts = av_rescale_q(ts, AV_TIME_BASE_Q, vst_->time_base);
        int idx = av_index_search_timestamp(vst_, vts, AVSEEK_FLAG_BACKWARD);
        if (idx >= 0)
            if (const AVIndexEntry* e = avformat_index_get_entry(vst_, idx)) keyframe = e->timestamp;
    }

    const bool track_changes = sink_ && r.track != worker_track_;
    bool keep_going = !worker_eof_ && !track_changes && read_pts_ >= 0 && r.target > read_pts_ + 0.05;
    if (keep_going && has_video_decoder())
        keep_going = last_video_pkt_pts_ != AV_NOPTS_VALUE &&
                     (keyframe == AV_NOPTS_VALUE || keyframe <= last_video_pkt_pts_);

    if (keep_going) {
        // Discard output still waiting in the decoders (all before the target);
        // their state stays valid for the packets that follow.
        FramePtr tmp(av_frame_alloc());
        if (has_video_decoder())
            while (tmp && video_receive(tmp.get()) == 0) av_frame_unref(tmp.get());
        if (adec_)
            while (tmp && avcodec_receive_frame(adec_.get(), tmp.get()) == 0) av_frame_unref(tmp.get());
        video_skip_until_ = AV_NOPTS_VALUE;
    } else {
        if (track_changes) open_audio_decoder(r.track);
        if (has_video_decoder()) video_flush();
        if (adec_) avcodec_flush_buffers(adec_.get());
        if (av_seek_frame(in_.get(), -1, ts, AVSEEK_FLAG_BACKWARD) < 0)
            av_seek_frame(in_.get(), -1, 0, AVSEEK_FLAG_BACKWARD);
        video_skip_until_ = keyframe;
        read_pts_ = -1;
        last_video_pkt_pts_ = AV_NOPTS_VALUE;
        worker_eof_ = false;
        ++rewinds_;
    }
    swr_free(&swr_);

    double half_frame = setup_.fps > 0 ? 0.5 / setup_.fps : 0.02;
    video_drop_before_ = r.target > 0 ? r.target - half_frame : -1;
    audio_drop_before_ = r.target > 0 ? r.target : -1;
    audio_next_pts_ = -1;
}

FramePtr MediaSource::to_yuv420p(FramePtr f) {
    auto fmt = static_cast<AVPixelFormat>(f->format);
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVA420P || fmt == AV_PIX_FMT_YUVJ420P) return f;
    sws_ = sws_getCachedContext(sws_, f->width, f->height, fmt, f->width, f->height, AV_PIX_FMT_YUV420P,
                                SWS_BILINEAR, nullptr, nullptr, nullptr);
    FramePtr out(av_frame_alloc());
    if (!sws_ || !out) return nullptr;
    out->format = AV_PIX_FMT_YUV420P;
    out->width = f->width;
    out->height = f->height;
    if (av_frame_get_buffer(out.get(), 0) < 0) return nullptr;
    sws_scale(sws_, f->data, f->linesize, 0, f->height, out->data, out->linesize);
    return out;
}

void MediaSource::decode_video(const AVPacket* pkt) {
    if (pkt && video_skip_until_ != AV_NOPTS_VALUE) {
        if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < video_skip_until_) return;
        video_skip_until_ = AV_NOPTS_VALUE;
    }
    if (video_send(pkt) < 0 && pkt) return;  // damaged packet: skip it
    FramePtr f(av_frame_alloc());
    while (f && video_receive(f.get()) == 0) {
        if (f->best_effort_timestamp == AV_NOPTS_VALUE) {
            av_frame_unref(f.get());
            continue;
        }
        double pts = static_cast<double>(f->best_effort_timestamp) * av_q2d(vst_->time_base);
        if (pts < video_drop_before_) {
            av_frame_unref(f.get());
            continue;
        }
        FramePtr out = to_yuv420p(std::move(f));
        if (out && !push_video(std::move(out), pts)) return;  // interrupted by a seek or quit
        f.reset(av_frame_alloc());
    }
}

int MediaSource::video_send(const AVPacket* pkt) {
    return b2dec_ ? b2dec_->send_packet(pkt) : avcodec_send_packet(vdec_.get(), pkt);
}

int MediaSource::video_receive(AVFrame* frame) {
    return b2dec_ ? b2dec_->receive_frame(frame) : avcodec_receive_frame(vdec_.get(), frame);
}

void MediaSource::video_flush() {
    if (b2dec_)
        b2dec_->flush();
    else
        avcodec_flush_buffers(vdec_.get());
}

void MediaSource::decode_audio(const AVPacket* pkt) {
    const AVStream* st = audio_streams_[worker_track_];
    if (pkt && pkt->pts != AV_NOPTS_VALUE && audio_drop_before_ > 0 &&
        static_cast<double>(pkt->pts) * av_q2d(st->time_base) < audio_drop_before_ - 1.0)
        return;  // well before the seek target: not worth decoding
    if (avcodec_send_packet(adec_.get(), pkt) < 0 && pkt) return;

    FramePtr f(av_frame_alloc());
    while (f && avcodec_receive_frame(adec_.get(), f.get()) == 0) {
        const int channels = f->ch_layout.nb_channels;
        const int rate = f->sample_rate;
        if (!swr_) {
            AVChannelLayout in_layout{}, out_layout{};
            if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&in_layout, channels);
            else
                av_channel_layout_copy(&in_layout, &f->ch_layout);
            av_channel_layout_default(&out_layout, channels);
            int ret = swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, rate, &in_layout,
                                          static_cast<AVSampleFormat>(f->format), rate, 0, nullptr);
            av_channel_layout_uninit(&in_layout);
            av_channel_layout_uninit(&out_layout);
            if (ret < 0 || swr_init(swr_) < 0) {
                swr_free(&swr_);
                return;
            }
            if (channels != sink_channels_ || rate != sink_rate_) {
                std::lock_guard lk(mu_);
                if (sink_->configure(rate, channels)) {
                    sink_channels_ = channels;
                    sink_rate_ = rate;
                }
            }
        }

        int max_out = swr_get_out_samples(swr_, f->nb_samples);
        pcm_.resize(static_cast<size_t>(std::max(max_out, 0)) * channels);
        uint8_t* out[1] = {reinterpret_cast<uint8_t*>(pcm_.data())};
        int n = swr_convert(swr_, out, max_out, const_cast<const uint8_t**>(f->extended_data), f->nb_samples);
        // Bink audio packets can decode to several frames, and only the first
        // one carries a timestamp; continue the others from the sample count.
        double start = NAN;
        if (f->best_effort_timestamp != AV_NOPTS_VALUE)
            start = static_cast<double>(f->best_effort_timestamp) * av_q2d(st->time_base);
        else if (audio_next_pts_ >= 0)
            start = audio_next_pts_;
        if (!std::isnan(start)) audio_next_pts_ = start + static_cast<double>(f->nb_samples) / rate;
        av_frame_unref(f.get());
        if (n <= 0) continue;

        const float* data = pcm_.data();
        if (!std::isnan(start) && start < audio_drop_before_) {
            int skip = static_cast<int>(std::min<int64_t>(n, std::llround((audio_drop_before_ - start) * rate)));
            data += static_cast<size_t>(skip) * channels;
            n -= skip;
            start += static_cast<double>(skip) / rate;
        }
        if (n <= 0) continue;
        if (!push_audio(data, n, start)) return;
        audio_drop_before_ = -1;
    }
}

bool MediaSource::push_video(FramePtr frame, double pts) {
    std::unique_lock lk(mu_);
    cv_.wait(lk, [&] { return quit_ || pending_ || vq_.size() < kVideoQueueFrames; });
    if (quit_ || pending_) return false;
    vq_.push_back({std::move(frame), pts});
    seek_active_ = false;
    return true;
}

bool MediaSource::push_audio(const float* data, int samples, double start) {
    std::unique_lock lk(mu_);
    while (true) {
        if (quit_ || pending_) return false;
        if (sink_->buffered_seconds() < kMaxAudioBuffer) break;
        seek_active_ = false;  // audio is full and still no picture: let the clock run
        cv_.wait_for(lk, std::chrono::milliseconds(10));
    }
    if (audio_rebase_) {
        if (!std::isnan(start)) audio_end_ = start;
        audio_rebase_ = false;
    }
    sink_->write(data, samples);
    audio_end_ += static_cast<double>(samples) / sink_rate_;
    // With video, the clock starts on the first frame (Bink audio comes first in the file).
    if (!has_video_decoder()) seek_active_ = false;
    return true;
}

}  // namespace quillreel
