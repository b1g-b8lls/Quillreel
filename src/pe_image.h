// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal loader for 64-bit Windows DLLs (PE32+), enough to run a
// self-contained decoder library such as RAD's bink2w64.dll inside this
// process. Imports are bound to functions supplied by the caller (see
// win32_shim.h); nothing from Windows or Wine is needed.
// Design notes: docs/bink2.md
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace quillreel::pe {

// Returns the address to bind for DLL!name. Must never return null; unknown
// functions get a stub (see win32_shim.cpp).
using ImportResolver = std::function<void*(std::string_view dll, std::string_view name)>;

struct ImageInfo {
    uint16_t machine = 0;
    bool is_dll = false;
    bool is_pe32_plus = false;
};

// Reads just the headers, without loading anything.
std::expected<ImageInfo, std::string> probe(const std::string& path);

class Image {
public:
    // Maps the file, applies relocations, binds imports (including delay-load
    // imports) and sets page protections. Does not run any of its code.
    static std::expected<std::unique_ptr<Image>, std::string> load(const std::string& path,
                                                                   const ImportResolver& resolve);
    ~Image();
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;

    // Runs DllMain(DLL_PROCESS_ATTACH). Returns its result.
    bool attach();

    void* export_address(std::string_view name) const;
    const std::map<std::string, uint32_t, std::less<>>& exports() const { return exports_; }
    void* base() const { return base_; }
    size_t size() const { return size_; }
    const std::string& path() const { return path_; }

private:
    Image() = default;

    std::string path_;
    void* base_ = nullptr;
    size_t size_ = 0;
    uint32_t entry_rva_ = 0;
    std::map<std::string, uint32_t, std::less<>> exports_;  // name -> RVA
};

}  // namespace quillreel::pe
