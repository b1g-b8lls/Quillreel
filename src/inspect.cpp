// SPDX-License-Identifier: GPL-3.0-or-later
#include "inspect.h"

#include <string_view>

#include "av_util.h"

namespace quillreel {
namespace {

VideoInfo describe_video(const AVStream* st) {
    const AVCodecParameters* par = st->codecpar;
    VideoInfo v;
    v.fourcc = fourcc_string(par->codec_tag);
    std::string_view tag = v.fourcc;
    if (tag.starts_with("BIK"))
        v.generation = "Bink 1";
    else if (tag.starts_with("KB2"))
        v.generation = "Bink 2";
    else
        v.generation = "unknown";
    v.revision = v.fourcc[3];
    v.width = par->width;
    v.height = par->height;

    AVRational fps = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    v.frame_rate = {fps.num, fps.den};

    // The Bink demuxer stores the header's frame count as the stream duration.
    if (st->nb_frames > 0)
        v.frame_count = st->nb_frames;
    else if (st->duration != AV_NOPTS_VALUE && fps.num > 0)
        v.frame_count = av_rescale_q(st->duration, st->time_base, av_inv_q(fps));

    int entries = avformat_index_get_entries_count(st);
    if (entries > 0) {
        int64_t keys = 0;
        for (int i = 0; i < entries; ++i) {
            const AVIndexEntry* e = avformat_index_get_entry(const_cast<AVStream*>(st), i);
            if (e && (e->flags & AVINDEX_KEYFRAME)) ++keys;
        }
        v.keyframe_count = keys;
    }

    if (const AVCodec* dec = avcodec_find_decoder(par->codec_id)) v.decoder = dec->name;
    return v;
}

AudioTrack describe_audio(const AVStream* st) {
    const AVCodecParameters* par = st->codecpar;
    AudioTrack a;
    a.stream_index = st->index;
    a.track_id = st->id;
    a.codec = avcodec_get_name(par->codec_id);
    a.sample_rate = par->sample_rate;
    a.channels = par->ch_layout.nb_channels;
    a.decodable = avcodec_find_decoder(par->codec_id) != nullptr;
    return a;
}

}  // namespace

std::expected<FileInfo, Error> inspect_file(const std::string& path) {
    AVFormatContext* raw = nullptr;
    int ret = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (ret < 0) return std::unexpected(Error{ErrorKind::Open, av_error_string(ret)});
    InputPtr fmt(raw);

    // The Bink demuxer fills in every field we report from the file header,
    // so avformat_find_stream_info (which reads and decodes packets) is not needed.
    if (std::string_view(fmt->iformat->name) != "bink")
        return std::unexpected(Error{
            ErrorKind::NotBink,
            std::string("not a Bink file (detected format: ") + fmt->iformat->name + ")"});

    FileInfo info;
    info.path = path;
    info.file_size = fmt->pb ? avio_size(fmt->pb) : 0;

    for (unsigned i = 0; i < fmt->nb_streams; ++i) {
        const AVStream* st = fmt->streams[i];
        switch (st->codecpar->codec_type) {
        case AVMEDIA_TYPE_VIDEO:
            if (!info.video) info.video = describe_video(st);
            break;
        case AVMEDIA_TYPE_AUDIO:
            info.audio.push_back(describe_audio(st));
            break;
        default:
            break;
        }
    }

    // fmt->duration is only filled by avformat_find_stream_info, so derive the
    // duration from the header's frame count and frame rate instead.
    if (info.video && info.video->frame_rate.num > 0)
        info.duration_s = static_cast<double>(info.video->frame_count) * info.video->frame_rate.den /
                          info.video->frame_rate.num;
    if (info.duration_s > 0 && info.file_size > 0)
        info.bit_rate = static_cast<int64_t>(static_cast<double>(info.file_size) * 8 / info.duration_s);
    return info;
}

}  // namespace quillreel
