// SPDX-License-Identifier: GPL-3.0-or-later
#include "bink2_dll.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <vector>

extern "C" {
#include <libavutil/imgutils.h>
}

#include "pe_image.h"
#include "win32_shim.h"

#define WINAPI __attribute__((ms_abi))

namespace quillreel::bink2 {
namespace {

std::unexpected<Error> fail(ErrorKind kind, std::string message) {
    return std::unexpected(Error{kind, std::move(message)});
}

// The subset of RAD's Bink API that Quillreel calls. Signatures from RAD's
// public documentation; all are stable across the versions tested (1.992e from
// Fallout 4, 2020.07 from GTA V Enhanced).
struct Api {
    void*(WINAPI* open)(const char* name, uint32_t flags);
    int32_t(WINAPI* do_frame)(void* bink);
    void(WINAPI* next_frame)(void* bink);
    void(WINAPI* go_to)(void* bink, uint32_t frame, int32_t flags);  // frame is 1-based
    void(WINAPI* close)(void* bink);
    void(WINAPI* get_frame_buffers_info)(void* bink, void* info);
    void(WINAPI* register_frame_buffers)(void* bink, void* info);
    const char*(WINAPI* get_error)();
};

constexpr uint32_t kBinkNoFrameBuffers = 0x400;  // the caller allocates the frame buffers

struct Loaded {
    std::unique_ptr<pe::Image> image;
    Api api{};
};

std::expected<const Api*, Error> load(const std::string& path) {
    static std::mutex m;
    static auto* loaded = new std::map<std::string, Loaded>;  // kept for the life of the process
    std::lock_guard lk(m);
    std::error_code ec;
    std::string key = std::filesystem::weakly_canonical(path, ec).string();
    if (key.empty()) key = path;
    if (auto it = loaded->find(key); it != loaded->end()) return &it->second.api;

    if (!std::filesystem::is_regular_file(path, ec))
        return fail(ErrorKind::Unsupported, std::format("{}: no such file", path));
    auto image = pe::Image::load(path, win32::resolve_import);
    if (!image) return fail(ErrorKind::Unsupported, std::format("{}: {}", path, image.error()));
    Loaded l;
    l.image = std::move(*image);
    struct Need {
        const char* name;
        void* slot;
    };
    Need needs[] = {
        {"BinkOpen", &l.api.open},
        {"BinkDoFrame", &l.api.do_frame},
        {"BinkNextFrame", &l.api.next_frame},
        {"BinkGoto", &l.api.go_to},
        {"BinkClose", &l.api.close},
        {"BinkGetFrameBuffersInfo", &l.api.get_frame_buffers_info},
        {"BinkRegisterFrameBuffers", &l.api.register_frame_buffers},
        {"BinkGetError", &l.api.get_error},
    };
    for (const Need& n : needs) {
        void* fn = l.image->export_address(n.name);
        if (!fn)
            return fail(ErrorKind::Unsupported,
                        std::format("{}: not a Bink 2 decoder DLL (no {} function)", path, n.name));
        std::memcpy(n.slot, &fn, sizeof fn);
    }
    win32::finish_load(*l.image);
    if (!l.image->attach()) return fail(ErrorKind::Unsupported, std::format("{}: the DLL failed to initialise", path));
    auto [it, ok] = loaded->emplace(key, std::move(l));
    return &it->second.api;
}

std::expected<const Api*, Error> load_default() {
    auto path = dll_path();
    if (!path) return fail(ErrorKind::Unsupported, missing_message());
    return load(*path);
}

// BinkGetFrameBuffersInfo / BinkRegisterFrameBuffers exchange a struct whose
// layout changed between SDK versions (found by probing the DLLs and reading
// their code). Both start with the same 24-byte header:
//   u32 total_sets, y_width, y_height, c_width, c_height, current_set
// followed by the frame sets, each made of planes Y, cR, cB, A (and a fifth in
// newer versions):
//   1.992 (Fallout 4):      plane = {i32 allocate; void* buffer; u32 pitch}  24 bytes, 4 per set
//   2020.07 (GTA V Enh.):   plane = {void* buffer; i32 allocate; u32 pitch}  16 bytes, 5 per set
// The two are told apart by where the "allocate" flags of Y, cR and cB land.
struct Layout {
    size_t plane_size, planes_per_set, alloc_off, buffer_off, pitch_off;
    size_t set_size() const { return plane_size * planes_per_set; }
};
constexpr size_t kHeader = 24;
constexpr Layout kLayout1992{24, 4, 0, 8, 16};
constexpr Layout kLayout2020{16, 5, 8, 0, 12};

std::optional<Layout> detect_layout(const uint32_t* u) {
    if (u[6] == 1 && u[12] == 1 && u[18] == 1) return kLayout1992;
    if (u[8] == 1 && u[12] == 1 && u[16] == 1) return kLayout2020;
    return std::nullopt;
}

}  // namespace

std::string installed_path() {
    std::string base;
    if (const char* x = std::getenv("XDG_DATA_HOME"); x && *x == '/')
        base = x;
    else if (const char* h = std::getenv("HOME"); h && *h)
        base = std::string(h) + "/.local/share";
    else
        base = ".";
    return base + "/quillreel/bink2w64.dll";
}

std::optional<std::string> dll_path() {
    if (const char* e = std::getenv("QUILLREEL_BINK2_DLL"); e && *e) return std::string(e);
    std::error_code ec;
    std::string p = installed_path();
    if (std::filesystem::is_regular_file(p, ec)) return p;
    return std::nullopt;
}

std::string missing_message() {
    return "Bink 2 video is not enabled. It needs RAD's decoder, bink2w64.dll, which Quillreel does not "
           "include. Provide it from a game that uses Bink 2 with: quillreel bink2 install /path/to/bink2w64.dll";
}

std::string file_version(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::vector<char> f((std::istreambuf_iterator<char>(in)), {});
    static constexpr char16_t kKey[] = u"FileVersion";
    std::vector<char> key;
    for (char16_t c : kKey) key.insert(key.end(), {static_cast<char>(c & 0xff), static_cast<char>(c >> 8)});
    auto it = std::search(f.begin(), f.end(), key.begin(), key.end());  // includes the terminating 0
    if (it == f.end()) return {};
    size_t pos = static_cast<size_t>(it - f.begin()) + key.size();
    pos = (pos + 3) & ~size_t{3};  // the value starts on a 4-byte boundary
    std::string v;
    for (; pos + 1 < f.size() && v.size() < 64; pos += 2) {
        auto c = static_cast<char16_t>(static_cast<unsigned char>(f[pos]) | (static_cast<unsigned char>(f[pos + 1]) << 8));
        if (!c) break;
        v += c < 0x80 ? static_cast<char>(c) : '?';
    }
    while (!v.empty() && v.back() == ' ') v.pop_back();
    return v;
}

std::expected<DllInfo, Error> check(const std::string& path) {
    auto info = pe::probe(path);
    if (!info) return fail(ErrorKind::Unsupported, std::format("{}: {}", path, info.error()));
    if (info->machine != 0x8664 || !info->is_pe32_plus)
        return fail(ErrorKind::Unsupported,
                    std::format("{}: this is a 32-bit DLL; Quillreel needs the 64-bit bink2w64.dll", path));
    if (!info->is_dll) return fail(ErrorKind::Unsupported, std::format("{}: not a DLL", path));
    auto api = load(path);
    if (!api) return std::unexpected(api.error());
    return DllInfo{path, file_version(path)};
}

std::expected<DllInfo, Error> install(const std::string& path) {
    auto info = check(path);
    if (!info) return info;
    namespace fs = std::filesystem;
    std::string dest = installed_path();
    std::error_code ec;
    if (fs::exists(dest, ec) && fs::equivalent(path, dest, ec)) return DllInfo{dest, info->version};
    fs::create_directories(fs::path(dest).parent_path(), ec);
    if (ec) return fail(ErrorKind::Failed, std::format("cannot create {}: {}", fs::path(dest).parent_path().string(), ec.message()));
    fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail(ErrorKind::Failed, std::format("cannot copy to {}: {}", dest, ec.message()));
    fs::permissions(dest, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
                    ec);
    return DllInfo{dest, info->version};
}

std::expected<bool, Error> uninstall() {
    std::error_code ec;
    bool removed = std::filesystem::remove(installed_path(), ec);
    if (ec) return fail(ErrorKind::Failed, std::format("cannot remove {}: {}", installed_path(), ec.message()));
    return removed;
}

struct Decoder::Impl {
    const Api* api = nullptr;
    void* bink = nullptr;
    Layout layout{};
    alignas(16) uint8_t fb[1024] = {};  // the frame buffer struct registered with the DLL
    std::vector<void*> buffers;
    int64_t next_index = 0;  // frame the next BinkDoFrame decodes; -1 if unknown
    FramePtr pending;
    bool eof = false;

    uint8_t* plane(size_t set, size_t k) { return fb + kHeader + set * layout.set_size() + k * layout.plane_size; }
    int32_t& allocate(uint8_t* p) { return *reinterpret_cast<int32_t*>(p + layout.alloc_off); }
    uint8_t*& buffer(uint8_t* p) { return *reinterpret_cast<uint8_t**>(p + layout.buffer_off); }
    uint32_t& pitch(uint8_t* p) { return *reinterpret_cast<uint32_t*>(p + layout.pitch_off); }
    uint32_t header(int i) const {
        uint32_t v;
        std::memcpy(&v, fb + 4 * i, 4);
        return v;
    }

    ~Impl() {
        if (bink) api->close(bink);
        for (void* b : buffers) std::free(b);
    }
};

Decoder::Decoder(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Decoder::~Decoder() = default;

std::expected<std::unique_ptr<Decoder>, Error> Decoder::open(const std::string& path) {
    auto api = load_default();
    if (!api) return std::unexpected(api.error());
    auto impl = std::make_unique<Impl>();
    impl->api = *api;
    impl->bink = impl->api->open(path.c_str(), kBinkNoFrameBuffers);
    if (!impl->bink) {
        const char* why = impl->api->get_error();
        return fail(ErrorKind::Unsupported, std::format("bink2w64.dll cannot open {}: {}", path, why && *why ? why : "unknown error"));
    }
    // The handle starts with the public BINK struct: Width, Height, Frames, ...
    uint32_t head[3];
    std::memcpy(head, impl->bink, sizeof head);

    impl->api->get_frame_buffers_info(impl->bink, impl->fb);
    auto layout = detect_layout(reinterpret_cast<const uint32_t*>(impl->fb));
    if (!layout) return fail(ErrorKind::Unsupported, "this version of bink2w64.dll uses a frame buffer layout Quillreel doesn't know");
    impl->layout = *layout;
    const uint32_t sets = impl->header(0), yw = impl->header(1), yh = impl->header(2), cw = impl->header(3),
                   ch = impl->header(4);
    if (sets < 1 || kHeader + sets * layout->set_size() > sizeof impl->fb || yw < head[0] || yh < head[1] ||
        cw * 2 < head[0] || ch * 2 < head[1])
        return fail(ErrorKind::Unsupported, "bink2w64.dll reported unexpected frame buffer sizes");
    bool alpha = false;
    for (uint32_t s = 0; s < sets; ++s) {
        for (size_t k = 0; k < layout->planes_per_set; ++k) {
            uint8_t* p = impl->plane(s, k);
            if (!impl->allocate(p)) continue;
            bool chroma = k == 1 || k == 2;
            uint32_t w = chroma ? cw : yw, h = chroma ? ch : yh;
            uint32_t stride = (w + 63) & ~63u;
            void* mem = std::aligned_alloc(64, static_cast<size_t>(stride) * h);
            if (!mem) return fail(ErrorKind::Failed, "out of memory");
            std::memset(mem, 0, static_cast<size_t>(stride) * h);
            impl->buffers.push_back(mem);
            impl->buffer(p) = static_cast<uint8_t*>(mem);
            impl->pitch(p) = stride;
            if (k == 3) alpha = true;
        }
    }
    impl->api->register_frame_buffers(impl->bink, impl->fb);

    std::unique_ptr<Decoder> d(new Decoder(std::move(impl)));
    d->width_ = static_cast<int>(head[0]);
    d->height_ = static_cast<int>(head[1]);
    d->frames_ = head[2];
    d->alpha_ = alpha;
    return d;
}

int Decoder::send_packet(const AVPacket* pkt) {
    Impl& m = *impl_;
    if (!pkt) {
        m.eof = true;
        return 0;
    }
    if (m.pending) return AVERROR(EAGAIN);
    int64_t index = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : m.next_index;
    if (index < 0 || index >= frames_) return AVERROR_INVALIDDATA;
    if (index != m.next_index) m.api->go_to(m.bink, static_cast<uint32_t>(index + 1), 0);
    m.api->do_frame(m.bink);

    uint32_t set = m.header(5);
    if (set >= m.header(0)) return AVERROR_EXTERNAL;
    FramePtr f(av_frame_alloc());
    if (!f) return AVERROR(ENOMEM);
    f->format = pix_fmt();
    f->width = width_;
    f->height = height_;
    if (int ret = av_frame_get_buffer(f.get(), 0); ret < 0) return ret;
    // AVFrame planes are Y, U (= Bink's cB), V (= cR), A.
    static constexpr size_t kSource[4] = {0, 2, 1, 3};
    const int planes = alpha_ ? 4 : 3;
    for (int i = 0; i < planes; ++i) {
        uint8_t* p = m.plane(set, kSource[i]);
        int w = i == 1 || i == 2 ? (width_ + 1) / 2 : width_;
        int h = i == 1 || i == 2 ? (height_ + 1) / 2 : height_;
        av_image_copy_plane(f->data[i], f->linesize[i], m.buffer(p), static_cast<int>(m.pitch(p)), w, h);
    }
    f->pts = f->best_effort_timestamp = index;
    f->duration = 1;
    m.pending = std::move(f);

    if (index + 1 < frames_) {
        m.api->next_frame(m.bink);
        m.next_index = index + 1;
    } else {
        m.next_index = -1;  // past the end; the next request seeks
    }
    return 0;
}

int Decoder::receive_frame(AVFrame* frame) {
    Impl& m = *impl_;
    if (m.pending) {
        av_frame_move_ref(frame, m.pending.get());
        m.pending.reset();
        return 0;
    }
    return m.eof ? AVERROR_EOF : AVERROR(EAGAIN);
}

void Decoder::flush() {
    impl_->pending.reset();
    impl_->eof = false;
}

}  // namespace quillreel::bink2
