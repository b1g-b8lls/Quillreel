// SPDX-License-Identifier: GPL-3.0-or-later
// Stand-ins for the Windows API functions a self-contained decoder DLL
// (RAD's bink2w64.dll) imports: heap, files, critical sections, threads,
// semaphores, timers, and the C runtime's start-up queries. Window, GDI and
// sound functions are stubs; the decoder never needs them.
// Design notes: docs/bink2.md
#pragma once

#include <string>
#include <string_view>

namespace quillreel::pe {
class Image;
}

namespace quillreel::win32 {

// pe::ImportResolver for Image::load. Never returns null: functions without a
// stand-in get a stub that returns 0 (logged when QUILLREEL_BINK2_TRACE is set).
// The name "module:X.dll" asks for a module handle instead of a function.
void* resolve_import(std::string_view dll, std::string_view name);

// Call after Image::load and before Image::attach: makes the generated stubs
// executable and lets GetModuleHandle/GetProcAddress find the DLL itself.
void finish_load(const pe::Image& image);

}  // namespace quillreel::win32
