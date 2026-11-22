// SPDX-License-Identifier: GPL-3.0-or-later
#include "play_sdl.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "sdl_audio_sink.h"
#include "sync_clock.h"

namespace quillreel {
namespace {

std::string format_time(double s) {
    auto t = static_cast<long long>(std::max(0.0, s));
    return std::format("{}:{:02}:{:02}", t / 3600, t / 60 % 60, t % 60);
}

struct SdlSession {
    ~SdlSession() { SDL_Quit(); }
};
struct WindowCloser {
    void operator()(SDL_Window* w) const { SDL_DestroyWindow(w); }
};
struct RendererCloser {
    void operator()(SDL_Renderer* r) const { SDL_DestroyRenderer(r); }
};
struct TextureCloser {
    void operator()(SDL_Texture* t) const { SDL_DestroyTexture(t); }
};

class Player {
public:
    Player(std::unique_ptr<MediaSource> src, const PlayOptions& opts, const UiOptions& ui)
        : src_(std::move(src)), setup_(src_->setup()), opts_(opts), ui_(ui), volume_(ui.volume) {}

    // Members are declared so that the worker (in src_) stops before the audio
    // sink it writes to is destroyed, and SDL shuts down last.
    int run() {
        if (!init()) return static_cast<int>(ErrorKind::Failed);
        src_->start(sink_.get());
        loop();
        if (ui_.report)
            std::printf("report: shown=%lld dropped=%lld first_pts=%.3f last_pts=%.3f audio_clock=%.3f track=%d "
                        "rewinds=%d\n",
                        shown_, dropped_, first_pts_, last_pts_, sink_ ? src_->audio_clock() : 0.0,
                        src_->audio_track(), src_->rewinds());
        return 0;
    }

private:
    bool init() {
        SDL_SetAppMetadata("Quillreel", nullptr, nullptr);
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            std::cerr << "quillreel: cannot open a window: " << SDL_GetError() << "\n";
            return false;
        }
        sdl_ = std::make_unique<SdlSession>();

        for (const auto& [at, spec] : ui_.test_keys) {
            bool shift = spec.starts_with("shift+");
            std::string name = shift ? spec.substr(6) : spec;
            // "," separates the list, so the frame-step keys go by name.
            if (name == "Comma") name = ",";
            if (name == "Period") name = ".";
            SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
            if (key == SDLK_UNKNOWN) {
                std::cerr << "quillreel: unknown key name in --test-keys: " << spec << "\n";
                return false;
            }
            test_keys_.push_back({at, key, shift});
        }

        if (setup_.has_audio) {
            std::string why;
            sink_ = SdlAudioSink::open(setup_.sample_rate, setup_.channels, &why);
            if (!sink_) std::cerr << "warning: no audio output (" << why << "); playing without sound\n";
        }
        if (!setup_.has_video && !sink_) {
            std::cerr << "quillreel: no audio output and no video to show\n";
            return false;
        }
        audio_only_ = !setup_.has_video;

        int w = 640, h = 140;
        if (!audio_only_) {
            double dw = setup_.width * av_q2d(setup_.sample_aspect);
            double dh = setup_.height;
            double scale = 1;
            SDL_Rect usable;
            if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &usable) && usable.w > 0 && usable.h > 0)
                scale = std::min({1.0, 0.9 * usable.w / dw, 0.9 * usable.h / dh});
            w = std::max(1, static_cast<int>(dw * scale));
            h = std::max(1, static_cast<int>(dh * scale));
        }
        std::string title = std::filesystem::path(opts_.input).filename().string() + " - Quillreel";
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | (ui_.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        window_.reset(SDL_CreateWindow(title.c_str(), w, h, flags));
        if (window_) renderer_.reset(SDL_CreateRenderer(window_.get(), nullptr));
        if (!renderer_) {
            std::cerr << "quillreel: cannot open a window: " << SDL_GetError() << "\n";
            return false;
        }
        SDL_SetRenderVSync(renderer_.get(), 1);

        if (sink_) {
            sink_->set_gain(volume_ / 100.0f);
            sink_->set_paused(false);
        }
        clock_.set(opts_.start);
        if (audio_only_) osd_until_ = UINT64_MAX;
        return true;
    }

    double master_clock() {
        if (src_->seeking()) {
            clock_.set(src_->seek_target());
        } else if (sink_ && !clock_.paused() && sink_->buffered_seconds() > 0) {
            clock_.follow(src_->audio_clock());
        }
        return clock_.now();
    }

    void upload(const AVFrame* f) {
        if (!texture_ || tex_w_ != f->width || tex_h_ != f->height) {
            SDL_Colorspace cs = SDL_COLORSPACE_BT601_LIMITED;
            if (f->colorspace == AVCOL_SPC_BT709)
                cs = f->color_range == AVCOL_RANGE_JPEG ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
            else if (f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P)
                cs = SDL_COLORSPACE_BT601_FULL;
            SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_IYUV);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, f->width);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, f->height);
            SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, cs);
            texture_.reset(SDL_CreateTextureWithProperties(renderer_.get(), props));
            SDL_DestroyProperties(props);
            tex_w_ = f->width;
            tex_h_ = f->height;
        }
        if (texture_)
            SDL_UpdateYUVTexture(texture_.get(), nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1],
                                 f->data[2], f->linesize[2]);
    }

    std::string status_line(double now) const {
        std::string s = format_time(now) + " / " + format_time(setup_.duration);
        if (clock_.paused()) s += "  paused";
        if (src_->seeking()) s += "  seeking...";
        return s;
    }

    std::string detail_line() const {
        std::string s;
        if (sink_ && setup_.audio_tracks > 0)
            s += std::format("audio {}/{}  ", src_->audio_track() + 1, setup_.audio_tracks);
        s += muted_ ? std::string("muted") : std::format("vol {}%", volume_);
        if (loop_) s += "  loop";
        return s;
    }

    void render(double now) {
        SDL_Renderer* r = renderer_.get();
        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        int ow = 0, oh = 0;
        SDL_GetCurrentRenderOutputSize(r, &ow, &oh);

        if (texture_ && ow > 0 && oh > 0) {
            double aspect = tex_w_ * av_q2d(setup_.sample_aspect) / tex_h_;
            float dw = static_cast<float>(ow), dh = static_cast<float>(ow / aspect);
            if (dh > oh) {
                dh = static_cast<float>(oh);
                dw = static_cast<float>(oh * aspect);
            }
            SDL_FRect dst{(ow - dw) / 2, (oh - dh) / 2, dw, dh};
            SDL_RenderTexture(r, texture_.get(), nullptr, &dst);
        }

        if (osd_visible()) {
            std::vector<std::string> lines;
            if (audio_only_) {
                lines.push_back(std::filesystem::path(opts_.input).filename().string());
                lines.push_back(setup_.video_note.empty() ? "audio only" : setup_.video_note);
            }
            lines.push_back(status_line(now));
            lines.push_back(detail_line());

            const float scale = oh >= 600 ? 3.0f : 2.0f;
            const float line_h = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 4;
            size_t longest = 0;
            for (const auto& l : lines) longest = std::max(longest, l.size());
            SDL_SetRenderScale(r, scale, scale);
            SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(r, 0, 0, 0, 170);
            SDL_FRect box{4, 4, longest * SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE + 8.0f, lines.size() * line_h + 6};
            SDL_RenderFillRect(r, &box);
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            for (size_t i = 0; i < lines.size(); ++i) SDL_RenderDebugText(r, 8, 8 + i * line_h, lines[i].c_str());
            SDL_SetRenderScale(r, 1, 1);
        }
        SDL_RenderPresent(r);
        last_osd_text_ = osd_visible() ? status_line(now) + detail_line() : "";
    }

    bool osd_visible() const { return clock_.paused() || SDL_GetTicksNS() < osd_until_; }
    void show_osd() {
        if (!audio_only_) osd_until_ = SDL_GetTicksNS() + 2'000'000'000ULL;
        dirty_ = true;
    }

    void seek_to(double t) {
        src_->seek(t);
        clock_.set(src_->seek_target());
        show_osd();
    }

    void set_paused(bool paused) {
        clock_.set_paused(paused);
        if (sink_) sink_->set_paused(paused);
    }

    // , and . (YouTube's keys): pause, then show the previous or next frame.
    void step_frame(int direction) {
        if (audio_only_ || !setup_.has_video || setup_.fps <= 0 || src_->seeking()) return;
        set_paused(true);
        show_due_frames(clock_.now());  // what's on screen must be the last frame handed out
        if (direction > 0) {
            double pts = 0;
            if (!src_->peek_video(0, pts)) return;  // not decoded yet, or the end
            FramePtr f = src_->pop_video();
            if (!f) return;
            show_frame(f.get(), pts);
        } else {
            VideoFrame v;
            if (src_->step_back(v)) {
                show_frame(v.frame.get(), v.pts);
            } else if (shown_ > 0 && last_pts_ > 0) {
                // Before the remembered frames: decode up to the previous one
                // (between its time and the next, so the paused clock shows it).
                seek_to(last_pts_ - 0.75 / setup_.fps);
                return;
            } else {
                return;
            }
        }
        src_->align_audio(clock_.now());
        show_osd();
    }

    void show_frame(const AVFrame* f, double pts) {
        upload(f);
        last_pts_ = pts;
        ++shown_;
        clock_.set(pts);
        dirty_ = true;
    }

    // 0-9: jump to 0-90 % of the file.
    void seek_percent(int tenths) {
        if (setup_.duration > 0) seek_to(setup_.duration * tenths / 10.0);
    }

    void set_volume(int v) {
        volume_ = std::clamp(v, 0, 100);
        muted_ = false;
        if (sink_) sink_->set_gain(volume_ / 100.0f);
        show_osd();
    }

    void toggle_fullscreen() {
        bool fs = SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN;
        SDL_SetWindowFullscreen(window_.get(), !fs);
    }

    // Returns false when the user asked to quit.
    bool handle(const SDL_Event& ev, double now) {
        switch (ev.type) {
        case SDL_EVENT_QUIT:
            return false;
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            dirty_ = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            if (ev.button.clicks == 2) toggle_fullscreen();
            break;
        case SDL_EVENT_KEY_DOWN: {
            const bool shift = ev.key.mod & SDL_KMOD_SHIFT;
            switch (ev.key.key) {
            case SDLK_Q:
                return false;
            case SDLK_ESCAPE:
                if (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN)
                    SDL_SetWindowFullscreen(window_.get(), false);
                else
                    return false;
                break;
            case SDLK_SPACE:
            case SDLK_K:
                set_paused(!clock_.paused());
                show_osd();
                break;
            case SDLK_F:
                toggle_fullscreen();
                break;
            case SDLK_LEFT:
                seek_to(now - (shift ? 30 : 5));
                break;
            case SDLK_RIGHT:
                seek_to(now + (shift ? 30 : 5));
                break;
            case SDLK_J:
                seek_to(now - 10);
                break;
            case SDLK_L:
                seek_to(now + 10);
                break;
            case SDLK_COMMA:
                step_frame(-1);
                break;
            case SDLK_PERIOD:
                step_frame(1);
                break;
            case SDLK_HOME:
                seek_to(0);
                break;
            case SDLK_END:
                seek_to(setup_.duration > 0 ? setup_.duration : now);
                break;
            case SDLK_0: case SDLK_1: case SDLK_2: case SDLK_3: case SDLK_4:
            case SDLK_5: case SDLK_6: case SDLK_7: case SDLK_8: case SDLK_9:
                seek_percent(static_cast<int>(ev.key.key - SDLK_0));
                break;
            case SDLK_UP:
                set_volume(volume_ + 10);
                break;
            case SDLK_DOWN:
                set_volume(volume_ - 10);
                break;
            case SDLK_M:
                muted_ = !muted_;
                if (sink_) sink_->set_gain(muted_ ? 0.0f : volume_ / 100.0f);
                show_osd();
                break;
            case SDLK_A:
                if (sink_ && setup_.audio_tracks > 1) {
                    int n = setup_.audio_tracks;
                    int t = (src_->audio_track() + (shift ? n - 1 : 1)) % n;
                    src_->select_audio_track(t, now);
                    clock_.set(src_->seek_target());
                }
                show_osd();
                break;
            case SDLK_R:  // "repeat"; L is YouTube's 10 s forward
                loop_ = !loop_;
                show_osd();
                break;
            default:
                break;
            }
            break;
        }
        default:
            break;
        }
        return true;
    }

    // Shows the newest frame that is due, dropping any that are already late.
    // Returns when the next one is due, or -1.
    double show_due_frames(double now) {
        double pts0 = 0, pts1 = 0;
        while (src_->peek_video(0, pts0)) {
            if (pts0 > now) return pts0;
            if (src_->peek_video(1, pts1) && pts1 <= now) {
                src_->pop_video();
                ++dropped_;
                continue;
            }
            FramePtr f = src_->pop_video();
            if (!f) break;
            upload(f.get());
            if (shown_ == 0) first_pts_ = pts0;
            last_pts_ = pts0;
            ++shown_;
            dirty_ = true;
        }
        return -1;
    }

    void loop() {
        loop_ = ui_.loop;
        const Uint64 started = SDL_GetTicksNS();
        size_t next_key = 0;
        while (true) {
            while (next_key < test_keys_.size() &&
                   static_cast<double>(SDL_GetTicksNS() - started) / 1e9 >= test_keys_[next_key].at) {
                SDL_Event ev{};
                ev.type = SDL_EVENT_KEY_DOWN;
                ev.key.key = test_keys_[next_key].key;
                ev.key.mod = test_keys_[next_key].shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
                ++next_key;
                if (!handle(ev, master_clock())) return;
            }

            double now = master_clock();
            const double next_due = show_due_frames(now);

            if (dirty_ || (osd_visible() && last_osd_text_ != status_line(now) + detail_line()) ||
                (!osd_visible() && !last_osd_text_.empty())) {
                render(now);
                dirty_ = false;
            }

            // End of file: wait for the audio to drain and the last frame's duration.
            bool video_done = !setup_.has_video || shown_ == 0 || setup_.fps <= 0 || now >= last_pts_ + 1 / setup_.fps;
            if (src_->finished() && (!sink_ || sink_->buffered_seconds() < 0.005) && video_done && !clock_.paused()) {
                if (!loop_) break;
                seek_to(0);
                continue;
            }

            int timeout_ms = audio_only_ ? 50 : 10;
            if (next_due >= 0) timeout_ms = std::clamp(static_cast<int>((next_due - now) * 1000), 0, 10);
            SDL_Event ev;
            if (SDL_WaitEventTimeout(&ev, timeout_ms)) {
                do {
                    if (!handle(ev, master_clock())) return;
                } while (SDL_PollEvent(&ev));
            }
        }
    }

    std::unique_ptr<SdlSession> sdl_;  // destroyed last: SDL_Quit
    std::unique_ptr<SdlAudioSink> sink_;
    std::unique_ptr<MediaSource> src_;  // its worker writes to sink_
    std::unique_ptr<SDL_Window, WindowCloser> window_;
    std::unique_ptr<SDL_Renderer, RendererCloser> renderer_;
    std::unique_ptr<SDL_Texture, TextureCloser> texture_;
    const StreamSetup& setup_;
    const PlayOptions& opts_;
    const UiOptions& ui_;

    struct TestKey {
        double at;
        SDL_Keycode key;
        bool shift;
    };
    std::vector<TestKey> test_keys_;

    SyncClock clock_;
    bool audio_only_ = false;
    bool loop_ = false;
    bool muted_ = false;
    bool dirty_ = true;
    int volume_;
    int tex_w_ = 0, tex_h_ = 0;
    Uint64 osd_until_ = 0;
    std::string last_osd_text_;
    long long shown_ = 0, dropped_ = 0;
    double first_pts_ = -1, last_pts_ = -1;
};

}  // namespace

int play_file(const PlayOptions& opts, const UiOptions& ui) {
    auto src = MediaSource::open(opts);
    if (!src) {
        std::cerr << "quillreel: " << src.error().message << "\n";
        return static_cast<int>(src.error().kind);
    }
    if (!(*src)->setup().video_note.empty()) std::cerr << "note: " << (*src)->setup().video_note << "\n";
    Player player(std::move(*src), opts, ui);
    return player.run();
}

}  // namespace quillreel
