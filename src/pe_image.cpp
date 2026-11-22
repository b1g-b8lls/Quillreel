// SPDX-License-Identifier: GPL-3.0-or-later
#include "pe_image.h"

#include <sys/mman.h>
#include <unistd.h>

#include <cstring>
#include <format>
#include <fstream>
#include <iterator>
#include <vector>

namespace quillreel::pe {
namespace {

// Offsets from the PE/COFF specification.
constexpr uint16_t kMachineAmd64 = 0x8664;
constexpr uint16_t kMagicPe32Plus = 0x20b;
constexpr uint16_t kFileDll = 0x2000;
constexpr uint32_t kScnExecute = 0x20000000;
constexpr uint32_t kScnWrite = 0x80000000;
enum Dir { kExport = 0, kImport = 1, kReloc = 5, kDelayImport = 13 };

template <class T>
T rd(const uint8_t* p) {
    T v;
    std::memcpy(&v, p, sizeof v);
    return v;
}

struct Headers {
    uint32_t pe = 0;        // offset of "PE\0\0"
    uint16_t machine = 0;
    uint16_t sections = 0;
    uint16_t characteristics = 0;
    uint32_t opt = 0;       // offset of the optional header
    uint16_t magic = 0;
    uint32_t section_table = 0;
};

std::expected<Headers, std::string> parse_headers(const std::vector<uint8_t>& f) {
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return std::unexpected("not a Windows executable (no MZ header)");
    Headers h;
    h.pe = rd<uint32_t>(&f[0x3c]);
    if (h.pe > f.size() - 24 || std::memcmp(&f[h.pe], "PE\0\0", 4) != 0)
        return std::unexpected("not a Windows executable (no PE header)");
    h.machine = rd<uint16_t>(&f[h.pe + 4]);
    h.sections = rd<uint16_t>(&f[h.pe + 6]);
    uint16_t opt_size = rd<uint16_t>(&f[h.pe + 20]);
    h.characteristics = rd<uint16_t>(&f[h.pe + 22]);
    h.opt = h.pe + 24;
    if (h.opt + 2 > f.size()) return std::unexpected("truncated PE header");
    h.magic = rd<uint16_t>(&f[h.opt]);
    h.section_table = h.opt + opt_size;
    if (static_cast<size_t>(h.section_table) + 40ull * h.sections > f.size())
        return std::unexpected("truncated section table");
    return h;
}

std::expected<std::vector<uint8_t>, std::string> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read {}", path));
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), {});
}

// MSVC's __chkstk reads the stack limit from the Windows thread block
// (mov r11, gs:[0x10]). There is no thread block here, so make it load 0
// instead: the probe loop is then skipped, which is safe because Linux stacks
// are either fully mapped (threads) or grow on demand (main thread).
int patch_chkstk(uint8_t* p, size_t n) {
    static constexpr uint8_t kLoad[] = {0x65, 0x4c, 0x8b, 0x1c, 0x25, 0x10, 0x00, 0x00, 0x00};
    static constexpr uint8_t kZero[] = {0x45, 0x31, 0xdb,                     // xor r11d, r11d
                                        0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};  // 6-byte nop
    int count = 0;
    for (size_t i = 0; i + sizeof kLoad <= n; ++i) {
        if (std::memcmp(p + i, kLoad, sizeof kLoad) == 0) {
            std::memcpy(p + i, kZero, sizeof kZero);
            ++count;
        }
    }
    return count;
}

}  // namespace

std::expected<ImageInfo, std::string> probe(const std::string& path) {
    auto f = read_file(path);
    if (!f) return std::unexpected(f.error());
    auto h = parse_headers(*f);
    if (!h) return std::unexpected(h.error());
    return ImageInfo{h->machine, (h->characteristics & kFileDll) != 0, h->magic == kMagicPe32Plus};
}

std::expected<std::unique_ptr<Image>, std::string> Image::load(const std::string& path, const ImportResolver& resolve) {
    auto file = read_file(path);
    if (!file) return std::unexpected(file.error());
    const std::vector<uint8_t>& f = *file;
    auto hdr = parse_headers(f);
    if (!hdr) return std::unexpected(hdr.error());
    const Headers& h = *hdr;
    if (h.machine != kMachineAmd64 || h.magic != kMagicPe32Plus)
        return std::unexpected("not a 64-bit (x86-64) Windows DLL; 32-bit DLLs such as bink2w32.dll can't be used");
    if (!(h.characteristics & kFileDll)) return std::unexpected("not a DLL");

    const uint8_t* opt = &f[h.opt];
    const uint32_t entry = rd<uint32_t>(opt + 16);
    const uint64_t image_base = rd<uint64_t>(opt + 24);
    const uint32_t section_align = rd<uint32_t>(opt + 32);
    const uint32_t image_size = rd<uint32_t>(opt + 56);
    const uint32_t headers_size = rd<uint32_t>(opt + 60);
    const uint32_t ndirs = rd<uint32_t>(opt + 108);
    auto dir = [&](int i) -> std::pair<uint32_t, uint32_t> {
        if (static_cast<uint32_t>(i) >= ndirs) return {0, 0};
        return {rd<uint32_t>(opt + 112 + 8 * i), rd<uint32_t>(opt + 116 + 8 * i)};
    };
    if (image_size == 0 || image_size > (1u << 30)) return std::unexpected("implausible image size");

    // Prefer the image's own base address (no relocation needed), else anywhere.
    void* want = reinterpret_cast<void*>(image_base);
    void* mem = mmap(want, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) mem = mmap(nullptr, image_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return std::unexpected("out of memory mapping the DLL");

    std::unique_ptr<Image> img(new Image);
    img->path_ = path;
    img->base_ = mem;
    img->size_ = image_size;
    img->entry_rva_ = entry;
    auto* base = static_cast<uint8_t*>(mem);
    auto in_image = [&](uint64_t rva, uint64_t len) { return rva <= image_size && len <= image_size - rva; };

    std::memcpy(base, f.data(), std::min<size_t>({headers_size, f.size(), image_size}));
    struct Section {
        uint32_t rva, size, flags;
    };
    std::vector<Section> sections;
    for (unsigned i = 0; i < h.sections; ++i) {
        const uint8_t* s = &f[h.section_table + 40 * i];
        uint32_t vsize = rd<uint32_t>(s + 8), rva = rd<uint32_t>(s + 12);
        uint32_t raw_size = rd<uint32_t>(s + 16), raw_off = rd<uint32_t>(s + 20);
        uint32_t flags = rd<uint32_t>(s + 36);
        uint32_t span = std::max(vsize, raw_size);
        if (!in_image(rva, span)) return std::unexpected("section outside the image");
        uint32_t copy = std::min(raw_size, vsize ? vsize : raw_size);
        if (copy) {
            if (raw_off > f.size() || copy > f.size() - raw_off) return std::unexpected("truncated section data");
            std::memcpy(base + rva, &f[raw_off], copy);
        }
        sections.push_back({rva, span, flags});
    }

    // Relocations.
    const int64_t delta = reinterpret_cast<int64_t>(base) - static_cast<int64_t>(image_base);
    if (delta) {
        auto [rva, size] = dir(kReloc);
        if (!rva) return std::unexpected("the DLL can't be moved (no relocations) and its address is taken");
        if (!in_image(rva, size)) return std::unexpected("bad relocation table");
        for (uint32_t off = 0; off + 8 <= size;) {
            uint32_t page = rd<uint32_t>(base + rva + off), block = rd<uint32_t>(base + rva + off + 4);
            if (block < 8 || off + block > size) break;
            for (uint32_t e = 8; e + 2 <= block; e += 2) {
                uint16_t v = rd<uint16_t>(base + rva + off + e);
                uint32_t target = page + (v & 0xfff);
                switch (v >> 12) {
                case 0: break;  // padding
                case 10:        // DIR64
                    if (!in_image(target, 8)) return std::unexpected("bad relocation");
                    { uint64_t x = rd<uint64_t>(base + target) + delta; std::memcpy(base + target, &x, 8); }
                    break;
                case 3:  // HIGHLOW
                    if (!in_image(target, 4)) return std::unexpected("bad relocation");
                    { uint32_t x = rd<uint32_t>(base + target) + static_cast<uint32_t>(delta); std::memcpy(base + target, &x, 4); }
                    break;
                default:
                    return std::unexpected(std::format("unsupported relocation type {}", v >> 12));
                }
            }
            off += block;
        }
    }

    auto cstr = [&](uint32_t rva) -> std::string_view {
        if (rva >= image_size) return {};
        const char* s = reinterpret_cast<const char*>(base + rva);
        return {s, strnlen(s, image_size - rva)};
    };
    // Fills an import address table from its name table.
    auto bind = [&](std::string_view dll, uint32_t names_rva, uint32_t iat_rva) -> bool {
        for (uint32_t i = 0;; ++i) {
            if (!in_image(names_rva + 8ull * i, 8) || !in_image(iat_rva + 8ull * i, 8)) return false;
            uint64_t thunk = rd<uint64_t>(base + names_rva + 8 * i);
            if (!thunk) return true;
            std::string name = (thunk >> 63) ? std::format("#{}", thunk & 0xffff)
                                             : std::string(cstr(static_cast<uint32_t>(thunk) + 2));
            void* fn = resolve(dll, name);
            std::memcpy(base + iat_rva + 8 * i, &fn, 8);
        }
    };

    if (auto [rva, size] = dir(kImport); rva) {
        for (uint32_t d = rva; in_image(d, 20); d += 20) {
            uint32_t names = rd<uint32_t>(base + d), dll = rd<uint32_t>(base + d + 12), iat = rd<uint32_t>(base + d + 16);
            if (!dll && !iat) break;
            if (!bind(cstr(dll), names ? names : iat, iat)) return std::unexpected("bad import table");
        }
    }
    // Delay-load imports are bound now, so the DLL's own lazy loader never runs.
    if (auto [rva, size] = dir(kDelayImport); rva) {
        for (uint32_t d = rva; in_image(d, 32); d += 32) {
            uint32_t attrs = rd<uint32_t>(base + d), dll = rd<uint32_t>(base + d + 4);
            uint32_t module = rd<uint32_t>(base + d + 8), iat = rd<uint32_t>(base + d + 12);
            uint32_t names = rd<uint32_t>(base + d + 16);
            if (!dll) break;
            if (!(attrs & 1)) return std::unexpected("old-style delay imports are not supported");
            if (!bind(cstr(dll), names, iat)) return std::unexpected("bad delay import table");
            if (module && in_image(module, 8)) {
                void* handle = resolve("", std::format("module:{}", cstr(dll)));
                std::memcpy(base + module, &handle, 8);
            }
        }
    }

    if (auto [rva, size] = dir(kExport); rva && in_image(rva, 40)) {
        uint32_t nfuncs = rd<uint32_t>(base + rva + 20), nnames = rd<uint32_t>(base + rva + 24);
        uint32_t funcs = rd<uint32_t>(base + rva + 28), names = rd<uint32_t>(base + rva + 32);
        uint32_t ords = rd<uint32_t>(base + rva + 36);
        for (uint32_t i = 0; i < nnames; ++i) {
            if (!in_image(names + 4ull * i, 4) || !in_image(ords + 2ull * i, 2)) break;
            uint16_t ord = rd<uint16_t>(base + ords + 2 * i);
            if (ord >= nfuncs || !in_image(funcs + 4ull * ord, 4)) continue;
            img->exports_.emplace(std::string(cstr(rd<uint32_t>(base + names + 4 * i))),
                                  rd<uint32_t>(base + funcs + 4 * ord));
        }
    }

    for (const Section& s : sections)
        if (s.flags & kScnExecute) patch_chkstk(base + s.rva, s.size);

    // Page protections: headers read-only, sections as declared.
    const long page = sysconf(_SC_PAGESIZE);
    if (section_align >= static_cast<uint32_t>(page)) {
        mprotect(base, std::min<size_t>(image_size, page), PROT_READ);
        for (const Section& s : sections) {
            size_t len = (s.size + page - 1) / page * page;
            int prot = PROT_READ | ((s.flags & kScnWrite) ? PROT_WRITE : 0) | ((s.flags & kScnExecute) ? PROT_EXEC : 0);
            if (len && mprotect(base + s.rva, std::min<size_t>(len, image_size - s.rva), prot) != 0)
                return std::unexpected("cannot set page protections");
        }
    } else if (mprotect(base, image_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        return std::unexpected("cannot make the DLL executable");
    }
    return img;
}

Image::~Image() {
    if (base_) munmap(base_, size_);
}

bool Image::attach() {
    if (!entry_rva_) return true;
    using DllMain = int(__attribute__((ms_abi)) *)(void*, uint32_t, void*);
    auto fn = reinterpret_cast<DllMain>(static_cast<uint8_t*>(base_) + entry_rva_);
    return fn(base_, 1 /* DLL_PROCESS_ATTACH */, nullptr) != 0;
}

void* Image::export_address(std::string_view name) const {
    auto it = exports_.find(name);
    return it == exports_.end() ? nullptr : static_cast<uint8_t*>(base_) + it->second;
}

}  // namespace quillreel::pe
