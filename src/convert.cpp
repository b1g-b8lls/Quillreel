// SPDX-License-Identifier: GPL-3.0-or-later
#include "convert.h"

#include <algorithm>
#include <filesystem>
#include <format>
#include <span>
#include <string_view>

#include "av_util.h"
#include "bink2_dll.h"
#include "output_format.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

namespace quillreel {
namespace {

using Status = std::expected<void, Error>;

std::unexpected<Error> fail(ErrorKind kind, std::string message) {
    return std::unexpected(Error{kind, std::move(message)});
}

std::unexpected<Error> av_fail(std::string_view what, int err) {
    return fail(ErrorKind::Failed, std::format("{}: {}", what, av_error_string(err)));
}

template <class T>
std::span<const T> supported(const AVCodecContext* ctx, const AVCodec* codec, AVCodecConfig cfg) {
    const void* values = nullptr;
    int count = 0;
    if (avcodec_get_supported_config(ctx, codec, cfg, 0, &values, &count) < 0 || !values) return {};
    return {static_cast<const T*>(values), static_cast<size_t>(count)};
}

std::string layout_name(const AVChannelLayout& layout) {
    char buf[256] = {};
    av_channel_layout_describe(&layout, buf, sizeof buf);
    return buf;
}

const AVCodec* find_encoder(const std::vector<std::string>& names) {
    for (const std::string& n : names)
        if (const AVCodec* c = avcodec_find_encoder_by_name(n.c_str())) return c;
    return nullptr;
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : " or ") + s;
    return out;
}

// One input stream on its way to one output stream.
struct Pipeline {
    AVStream* in = nullptr;
    int track = -1;  // audio track position, -1 for video
    CodecPtr dec;
    std::unique_ptr<bink2::Decoder> b2;  // Bink 2 video: RAD's decoder instead of dec
    CodecPtr enc;
    GraphPtr graph;  // built from the first decoded frame
    AVFilterContext* src = nullptr;
    AVFilterContext* sink = nullptr;
    AVStream* out = nullptr;
    int64_t next_audio_pts = 0;
    int64_t last_video_pts = AV_NOPTS_VALUE;
    int64_t written = 0;  // video frames or audio samples

    bool is_video() const { return track < 0; }
    int send(const AVPacket* pkt) { return b2 ? b2->send_packet(pkt) : avcodec_send_packet(dec.get(), pkt); }
    int receive(AVFrame* f) { return b2 ? b2->receive_frame(f) : avcodec_receive_frame(dec.get(), f); }
};

// Owns the output context; deletes a half-written output file unless commit() was called.
class Output {
public:
    explicit Output(std::string path) : path_(std::move(path)) {}
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    ~Output() {
        if (!ctx_) return;
        bool opened = ctx_->pb && !(ctx_->oformat->flags & AVFMT_NOFILE);
        if (opened) avio_closep(&ctx_->pb);
        avformat_free_context(ctx_);
        if (opened && !committed_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
    }
    AVFormatContext*& ctx() { return ctx_; }
    void commit() { committed_ = true; }

private:
    std::string path_;
    AVFormatContext* ctx_ = nullptr;
    bool committed_ = false;
};

class Converter {
public:
    Converter(const ConvertOptions& opts, const ProgressFn& progress) : opts_(opts), progress_(progress) {}

    std::expected<ConvertStats, Error> run() {
        if (auto s = open_input(); !s) return std::unexpected(s.error());
        auto fmt = output_format_for(opts_.output);
        if (!fmt)
            return fail(ErrorKind::Usage, std::format("don't know how to write '{}' (supported extensions: {})",
                                                      opts_.output, supported_extensions()));
        format_ = *fmt;
        if (auto s = check_output_path(); !s) return std::unexpected(s.error());
        if (auto s = select_streams(); !s) return std::unexpected(s.error());
        if (auto s = open_output(); !s) return std::unexpected(s.error());
        if (auto s = transcode(); !s) return std::unexpected(s.error());

        int ret = av_write_trailer(out_.ctx());
        if (ret < 0) return av_fail("finishing output", ret);
        out_.commit();

        ConvertStats stats;
        stats.output = opts_.output;
        stats.input_audio_tracks = static_cast<int>(audio_streams_.size());
        stats.decode_errors = decode_errors_;
        for (const Pipeline& p : pipes_) {
            if (p.is_video()) {
                stats.video_frames = p.written;
            } else {
                stats.audio_tracks.push_back(p.track);
                stats.audio_samples.push_back(p.written);
                stats.audio_rates.push_back(p.enc->sample_rate);
            }
        }
        return stats;
    }

private:
    Status open_input() {
        AVFormatContext* raw = nullptr;
        int ret = avformat_open_input(&raw, opts_.input.c_str(), nullptr, nullptr);
        if (ret < 0) return fail(ErrorKind::Open, std::format("{}: {}", opts_.input, av_error_string(ret)));
        in_.reset(raw);

        if (std::string_view(in_->iformat->name) == "bink") return {};
        if (!opts_.allow_non_bink)
            return fail(ErrorKind::NotBink,
                        std::format("{}: not a Bink file (detected format: {})", opts_.input, in_->iformat->name));
        // Generic inputs may need packets read before their parameters are known.
        // The Bink demuxer fills everything from the header, so Bink files skip this.
        ret = avformat_find_stream_info(in_.get(), nullptr);
        if (ret < 0) return fail(ErrorKind::Open, std::format("{}: {}", opts_.input, av_error_string(ret)));
        return {};
    }

    Status check_output_path() {
        std::string first_file = opts_.output;
        if (format_.image_sequence) {
            char buf[4096];
            if (av_get_frame_filename(buf, sizeof buf, opts_.output.c_str(), 1) < 0)
                return fail(ErrorKind::Usage,
                            std::format("'{}': an image sequence needs a frame number pattern, e.g. frame_%05d.png",
                                        opts_.output));
            first_file = buf;
        }
        std::error_code ec;
        if (!opts_.overwrite && std::filesystem::exists(first_file, ec))
            return fail(ErrorKind::Failed, std::format("'{}' already exists (use -y to overwrite)", first_file));
        return {};
    }

    Status select_streams() {
        AVStream* video = nullptr;
        for (unsigned i = 0; i < in_->nb_streams; ++i) {
            AVStream* st = in_->streams[i];
            if (st->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC)) {
                if (!video) video = st;
            } else if (st->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audio_streams_.push_back(st);
            }
        }
        if (video && video->duration != AV_NOPTS_VALUE)
            total_s_ = static_cast<double>(video->duration) * av_q2d(video->time_base);
        else if (in_->duration != AV_NOPTS_VALUE)
            total_s_ = static_cast<double>(in_->duration) / AV_TIME_BASE;

        const bool format_has_audio = !format_.audio_encoders.empty();
        if ((opts_.audio_track || opts_.all_audio) && !format_has_audio)
            return fail(ErrorKind::Usage, std::format(".{} output cannot hold audio", format_.extension));

        bool want_video = opts_.video && !format_.video_encoders.empty() && video;
        bool want_audio = opts_.audio && format_has_audio && !audio_streams_.empty();

        if (want_video) {
            std::string tag = fourcc_string(video->codecpar->codec_tag);
            if (tag.starts_with("KB2")) {
                auto dec = bink2::Decoder::open(opts_.input);
                if (!dec)
                    return fail(dec.error().kind,
                                dec.error().message + "\nUse --no-video to convert only the audio.");
                Pipeline& p = pipes_.emplace_back();
                p.in = video;
                p.b2 = std::move(*dec);
            } else if (!avcodec_find_decoder(video->codecpar->codec_id)) {
                return fail(ErrorKind::Unsupported,
                            std::format("FFmpeg has no decoder for this video codec ({}). "
                                        "Use --no-video to convert only the audio.",
                                        tag));
            } else {
                pipes_.emplace_back().in = video;
            }
        }

        if (opts_.audio_track && audio_streams_.empty())
            return fail(ErrorKind::Usage, "the input has no audio tracks");
        if (want_audio) {
            std::vector<int> tracks;
            if (opts_.all_audio) {
                if (!format_.multi_audio && audio_streams_.size() > 1)
                    return fail(ErrorKind::Usage,
                                std::format(".{} holds one audio track; pick one with --audio-track N (0-{})",
                                            format_.extension, audio_streams_.size() - 1));
                for (size_t i = 0; i < audio_streams_.size(); ++i) tracks.push_back(static_cast<int>(i));
            } else {
                int t = opts_.audio_track.value_or(0);
                if (t < 0 || t >= static_cast<int>(audio_streams_.size()))
                    return fail(ErrorKind::Usage, std::format("audio track {} does not exist (the input has {}: 0-{})",
                                                              t, audio_streams_.size(), audio_streams_.size() - 1));
                tracks.push_back(t);
            }
            for (int t : tracks) {
                AVStream* st = audio_streams_[t];
                if (!avcodec_find_decoder(st->codecpar->codec_id))
                    return fail(ErrorKind::Unsupported,
                                std::format("FFmpeg has no decoder for audio track {} ({})", t,
                                            avcodec_get_name(st->codecpar->codec_id)));
                Pipeline& p = pipes_.emplace_back();
                p.in = st;
                p.track = t;
            }
        }

        if (pipes_.empty())
            return fail(ErrorKind::Usage, std::format("nothing to convert: the options and .{} output leave no streams",
                                                      format_.extension));
        return {};
    }

    Status open_decoder(Pipeline& p) {
        if (p.b2) return {};  // opened in select_streams
        const AVCodec* codec = avcodec_find_decoder(p.in->codecpar->codec_id);
        p.dec.reset(avcodec_alloc_context3(codec));
        if (!p.dec) return fail(ErrorKind::Failed, "out of memory");
        int ret = avcodec_parameters_to_context(p.dec.get(), p.in->codecpar);
        if (ret < 0) return av_fail("decoder setup", ret);
        p.dec->pkt_timebase = p.in->time_base;
        ret = avcodec_open2(p.dec.get(), codec, nullptr);
        if (ret < 0) return av_fail(std::format("opening {} decoder", codec->name), ret);
        return {};
    }

    Status open_video_encoder(Pipeline& p, const AVCodec* codec) {
        const AVCodecContext* dec = p.dec.get();
        AVCodecContext* enc = p.enc.get();
        AVRational fps = stream_frame_rate(in_.get(), p.in);
        if (fps.num <= 0 || fps.den <= 0) fps = {25, 1};

        enc->width = p.b2 ? p.b2->width() : dec->width;
        enc->height = p.b2 ? p.b2->height() : dec->height;
        enc->sample_aspect_ratio = p.b2 ? AVRational{0, 1} : dec->sample_aspect_ratio;
        enc->time_base = av_inv_q(fps);
        enc->framerate = fps;

        AVPixelFormat src_fmt = p.b2                              ? p.b2->pix_fmt()
                              : dec->pix_fmt != AV_PIX_FMT_NONE ? dec->pix_fmt
                                                                : static_cast<AVPixelFormat>(p.in->codecpar->format);
        if (src_fmt == AV_PIX_FMT_NONE) src_fmt = AV_PIX_FMT_YUV420P;
        auto fmts = supported<AVPixelFormat>(enc, codec, AV_CODEC_CONFIG_PIX_FORMAT);
        if (fmts.empty()) {
            enc->pix_fmt = src_fmt;
        } else {
            std::vector<AVPixelFormat> list(fmts.begin(), fmts.end());
            list.push_back(AV_PIX_FMT_NONE);
            const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(src_fmt);
            bool alpha = d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
            enc->pix_fmt = avcodec_find_best_pix_fmt_of_list(list.data(), src_fmt, alpha, nullptr);
        }

        // Quality defaults: near-transparent output for archival/editing use.
        std::string_view name = codec->name;
        if (name == "libx264") {
            av_opt_set(enc->priv_data, "crf", "18", 0);
        } else if (name == "libvpx-vp9") {
            enc->bit_rate = 0;
            av_opt_set(enc->priv_data, "crf", "30", 0);
            av_opt_set(enc->priv_data, "row-mt", "1", 0);
            av_opt_set(enc->priv_data, "cpu-used", "4", 0);
        }
        return {};
    }

    Status open_audio_encoder(Pipeline& p, const AVCodec* codec) {
        AVCodecContext* dec = p.dec.get();
        AVCodecContext* enc = p.enc.get();

        auto rates = supported<int>(enc, codec, AV_CODEC_CONFIG_SAMPLE_RATE);
        if (rates.empty() || std::ranges::contains(rates, dec->sample_rate)) {
            enc->sample_rate = dec->sample_rate;
        } else {
            // Smallest supported rate at or above the source, else the highest one.
            int best = *std::ranges::max_element(rates);
            for (int r : rates)
                if (r >= dec->sample_rate && r < best) best = r;
            enc->sample_rate = best;
        }

        auto sfmts = supported<AVSampleFormat>(enc, codec, AV_CODEC_CONFIG_SAMPLE_FORMAT);
        enc->sample_fmt = (sfmts.empty() || std::ranges::contains(sfmts, dec->sample_fmt)) ? dec->sample_fmt : sfmts[0];

        AVChannelLayout src_layout{};
        if (dec->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC || !av_channel_layout_check(&dec->ch_layout))
            av_channel_layout_default(&src_layout, std::max(dec->ch_layout.nb_channels, 1));
        else
            av_channel_layout_copy(&src_layout, &dec->ch_layout);
        auto layouts = supported<AVChannelLayout>(enc, codec, AV_CODEC_CONFIG_CHANNEL_LAYOUT);
        const AVChannelLayout* chosen = &src_layout;
        if (!layouts.empty()) {
            const AVChannelLayout* same_count = nullptr;
            const AVChannelLayout* exact = nullptr;
            for (const AVChannelLayout& l : layouts) {
                if (!exact && av_channel_layout_compare(&l, &src_layout) == 0) exact = &l;
                if (!same_count && l.nb_channels == src_layout.nb_channels) same_count = &l;
            }
            chosen = exact ? exact : same_count ? same_count : &layouts[0];
        }
        int ret = av_channel_layout_copy(&enc->ch_layout, chosen);
        av_channel_layout_uninit(&src_layout);
        if (ret < 0) return av_fail("channel layout", ret);

        enc->time_base = {1, enc->sample_rate};
        const AVCodecDescriptor* desc = avcodec_descriptor_get(codec->id);
        if (desc && !(desc->props & AV_CODEC_PROP_LOSSLESS)) enc->bit_rate = 80000LL * enc->ch_layout.nb_channels;
        return {};
    }

    Status open_output() {
        AVFormatContext*& oc = out_.ctx();
        int ret = avformat_alloc_output_context2(&oc, nullptr, format_.muxer.c_str(), opts_.output.c_str());
        if (ret < 0) return av_fail("creating output", ret);

        for (Pipeline& p : pipes_) {
            if (auto s = open_decoder(p); !s) return s;
            const auto& names = p.is_video() ? format_.video_encoders : format_.audio_encoders;
            const AVCodec* codec = find_encoder(names);
            if (!codec)
                return fail(ErrorKind::Unsupported,
                            std::format("this FFmpeg build has no {} encoder for .{} output (needs {})",
                                        p.is_video() ? "video" : "audio", format_.extension, join(names)));
            p.enc.reset(avcodec_alloc_context3(codec));
            if (!p.enc) return fail(ErrorKind::Failed, "out of memory");

            Status s = p.is_video() ? open_video_encoder(p, codec) : open_audio_encoder(p, codec);
            if (!s) return s;
            if (oc->oformat->flags & AVFMT_GLOBALHEADER) p.enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            ret = avcodec_open2(p.enc.get(), codec, nullptr);
            if (ret < 0) return av_fail(std::format("opening {} encoder", codec->name), ret);

            p.out = avformat_new_stream(oc, nullptr);
            if (!p.out) return fail(ErrorKind::Failed, "out of memory");
            ret = avcodec_parameters_from_context(p.out->codecpar, p.enc.get());
            if (ret < 0) return av_fail("output stream setup", ret);
            p.out->time_base = p.enc->time_base;
            if (p.is_video()) p.out->avg_frame_rate = p.enc->framerate;
        }

        if (!(oc->oformat->flags & AVFMT_NOFILE)) {
            ret = avio_open(&oc->pb, opts_.output.c_str(), AVIO_FLAG_WRITE);
            if (ret < 0) return av_fail(std::format("cannot write '{}'", opts_.output), ret);
        }
        ret = avformat_write_header(oc, nullptr);
        if (ret < 0) return av_fail("writing output header", ret);
        return {};
    }

    // The filter graph converts whatever the decoder produces (pixel format,
    // sample format/rate, channel layout) into what the encoder accepts.
    Status build_filters(Pipeline& p, const AVFrame* f) {
        p.graph.reset(avfilter_graph_alloc());
        if (!p.graph) return fail(ErrorKind::Failed, "out of memory");

        std::string src_args, chain;
        const char *src_name, *sink_name;
        AVRational tb = p.in->time_base;
        if (p.is_video()) {
            AVRational sar = f->sample_aspect_ratio.num ? f->sample_aspect_ratio : AVRational{1, 1};
            src_args = std::format("video_size={}x{}:pix_fmt={}:time_base={}/{}:pixel_aspect={}/{}", f->width,
                                   f->height, f->format, tb.num, tb.den, sar.num, sar.den);
            chain = std::format("format=pix_fmts={}", av_get_pix_fmt_name(p.enc->pix_fmt));
            src_name = "buffer";
            sink_name = "buffersink";
        } else {
            AVChannelLayout layout{};
            if (f->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                av_channel_layout_default(&layout, f->ch_layout.nb_channels);
            else
                av_channel_layout_copy(&layout, &f->ch_layout);
            src_args = std::format("time_base={}/{}:sample_rate={}:sample_fmt={}:channel_layout={}", tb.num, tb.den,
                                   f->sample_rate, av_get_sample_fmt_name(static_cast<AVSampleFormat>(f->format)),
                                   layout_name(layout));
            av_channel_layout_uninit(&layout);
            chain = std::format("aresample={},aformat=sample_fmts={}:channel_layouts={}", p.enc->sample_rate,
                                av_get_sample_fmt_name(p.enc->sample_fmt), layout_name(p.enc->ch_layout));
            src_name = "abuffer";
            sink_name = "abuffersink";
        }

        AVFilterGraph* g = p.graph.get();
        int ret = avfilter_graph_create_filter(&p.src, avfilter_get_by_name(src_name), "in", src_args.c_str(),
                                               nullptr, g);
        if (ret < 0) return av_fail("filter source", ret);
        ret = avfilter_graph_create_filter(&p.sink, avfilter_get_by_name(sink_name), "out", nullptr, nullptr, g);
        if (ret < 0) return av_fail("filter sink", ret);

        AVFilterInOut* outputs = avfilter_inout_alloc();
        AVFilterInOut* inputs = avfilter_inout_alloc();
        if (!outputs || !inputs) {
            avfilter_inout_free(&outputs);
            avfilter_inout_free(&inputs);
            return fail(ErrorKind::Failed, "out of memory");
        }
        outputs->name = av_strdup("in");
        outputs->filter_ctx = p.src;
        inputs->name = av_strdup("out");
        inputs->filter_ctx = p.sink;
        ret = avfilter_graph_parse_ptr(g, chain.c_str(), &inputs, &outputs, nullptr);
        avfilter_inout_free(&inputs);
        avfilter_inout_free(&outputs);
        if (ret < 0) return av_fail("filter graph", ret);
        ret = avfilter_graph_config(g, nullptr);
        if (ret < 0) return av_fail("filter graph", ret);

        if (!p.is_video() && p.enc->frame_size > 0 &&
            !(p.enc->codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE))
            av_buffersink_set_frame_size(p.sink, p.enc->frame_size);
        return {};
    }

    Status encode(Pipeline& p, AVFrame* frame) {
        int ret = avcodec_send_frame(p.enc.get(), frame);
        if (ret < 0 && ret != AVERROR_EOF) return av_fail("encoding", ret);
        while (true) {
            ret = avcodec_receive_packet(p.enc.get(), pkt_.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return {};
            if (ret < 0) return av_fail("encoding", ret);
            pkt_->stream_index = p.out->index;
            av_packet_rescale_ts(pkt_.get(), p.enc->time_base, p.out->time_base);
            ret = av_interleaved_write_frame(out_.ctx(), pkt_.get());
            if (ret < 0) return av_fail("writing output", ret);
        }
    }

    Status drain_filters(Pipeline& p) {
        while (true) {
            int ret = av_buffersink_get_frame(p.sink, filtered_.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return {};
            if (ret < 0) return av_fail("filtering", ret);
            AVFrame* f = filtered_.get();
            if (p.is_video()) {
                int64_t pts = f->pts == AV_NOPTS_VALUE
                                ? (p.last_video_pts == AV_NOPTS_VALUE ? 0 : p.last_video_pts + 1)
                                : av_rescale_q(f->pts, av_buffersink_get_time_base(p.sink), p.enc->time_base);
                if (p.last_video_pts != AV_NOPTS_VALUE && pts <= p.last_video_pts) pts = p.last_video_pts + 1;
                p.last_video_pts = f->pts = pts;
                ++p.written;
            } else {
                // Audio timestamps come from the sample count, so they never drift or jump.
                f->pts = p.next_audio_pts;
                p.next_audio_pts += f->nb_samples;
                p.written += f->nb_samples;
            }
            f->pict_type = AV_PICTURE_TYPE_NONE;
            Status s = encode(p, f);
            av_frame_unref(f);
            if (!s) return s;
        }
    }

    // frame == nullptr signals end of stream to the filter graph.
    Status filter(Pipeline& p, AVFrame* frame) {
        if (frame && !p.graph)
            if (auto s = build_filters(p, frame); !s) return s;
        if (!p.graph) return {};  // stream ended without a single frame
        if (frame) frame->pts = frame->best_effort_timestamp;
        int ret = av_buffersrc_add_frame_flags(p.src, frame, 0);
        if (ret < 0) return av_fail("filtering", ret);
        return drain_filters(p);
    }

    // packet == nullptr flushes the decoder.
    Status decode(Pipeline& p, const AVPacket* packet) {
        int ret = p.send(packet);
        if (ret < 0 && ret != AVERROR_EOF) {
            ++decode_errors_;  // a damaged packet: skip it and keep going
            return {};
        }
        while (true) {
            ret = p.receive(frame_.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) return {};
            if (ret < 0) {
                ++decode_errors_;
                return {};
            }
            Status s = filter(p, frame_.get());
            av_frame_unref(frame_.get());
            if (!s) return s;
        }
    }

    Status transcode() {
        pkt_.reset(av_packet_alloc());
        frame_.reset(av_frame_alloc());
        filtered_.reset(av_frame_alloc());
        PacketPtr in_pkt(av_packet_alloc());
        if (!pkt_ || !frame_ || !filtered_ || !in_pkt) return fail(ErrorKind::Failed, "out of memory");

        std::vector<Pipeline*> by_stream(in_->nb_streams, nullptr);
        for (Pipeline& p : pipes_) by_stream[p.in->index] = &p;

        double last_report = -1;
        int ret;
        while ((ret = av_read_frame(in_.get(), in_pkt.get())) >= 0) {
            if (opts_.cancel && opts_.cancel->load(std::memory_order_relaxed)) {
                av_packet_unref(in_pkt.get());
                return fail(ErrorKind::Failed, "cancelled");
            }
            Pipeline* p = by_stream[in_pkt->stream_index];
            if (p) {
                if (progress_ && in_pkt->pts != AV_NOPTS_VALUE) {
                    double t = static_cast<double>(in_pkt->pts) * av_q2d(p->in->time_base);
                    if (t >= last_report + 0.25) {
                        progress_(t, total_s_);
                        last_report = t;
                    }
                }
                Status s = decode(*p, in_pkt.get());
                if (!s) return s;
            }
            av_packet_unref(in_pkt.get());
        }
        if (ret != AVERROR_EOF) return av_fail("reading input", ret);

        for (Pipeline& p : pipes_) {
            if (auto s = decode(p, nullptr); !s) return s;
            if (auto s = filter(p, nullptr); !s) return s;
            if (auto s = encode(p, nullptr); !s) return s;
        }
        if (progress_) progress_(total_s_, total_s_);
        return {};
    }

    const ConvertOptions& opts_;
    const ProgressFn& progress_;
    OutputFormat format_;
    InputPtr in_;
    std::vector<AVStream*> audio_streams_;
    std::vector<Pipeline> pipes_;
    Output out_{opts_.output};  // destroyed before pipes_ (reverse order); neither needs the other
    PacketPtr pkt_;
    FramePtr frame_;
    FramePtr filtered_;
    double total_s_ = 0;
    int64_t decode_errors_ = 0;
};

}  // namespace

std::expected<ConvertStats, Error> convert_file(const ConvertOptions& opts, const ProgressFn& progress) {
    Converter c(opts, progress);
    return c.run();
}

}  // namespace quillreel
