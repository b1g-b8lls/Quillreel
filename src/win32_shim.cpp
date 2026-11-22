// SPDX-License-Identifier: GPL-3.0-or-later
#include "win32_shim.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "pe_image.h"

#define WINAPI __attribute__((ms_abi))

namespace quillreel::win32 {
namespace {

using BOOL = int32_t;
using DWORD = uint32_t;
using LONG = int32_t;
using UINT = uint32_t;
using HANDLE = void*;
using WCHAR = char16_t;

constexpr DWORD kInfinite = 0xffffffff;
constexpr DWORD kWaitTimeout = 0x102;
constexpr DWORD kWaitFailed = 0xffffffff;
constexpr DWORD kErrorFileNotFound = 2;
constexpr DWORD kErrorInvalidHandle = 6;
constexpr DWORD kErrorNotEnoughMemory = 8;
constexpr DWORD kErrorInvalidParameter = 87;
constexpr DWORD kErrorInsufficientBuffer = 122;
constexpr DWORD kErrorModNotFound = 126;
constexpr DWORD kErrorProcNotFound = 127;
constexpr DWORD kErrorEnvvarNotFound = 203;
constexpr DWORD kErrorCallNotImplemented = 120;
const HANDLE kInvalidHandle = reinterpret_cast<HANDLE>(-1);
const HANDLE kCurrentProcess = reinterpret_cast<HANDLE>(-1);
const HANDLE kCurrentThread = reinterpret_cast<HANDLE>(-2);

bool tracing() {
    static const bool on = std::getenv("QUILLREEL_BINK2_TRACE") != nullptr;
    return on;
}

template <class... Args>
void trace(std::format_string<Args...> fmt, Args&&... args) {
    if (tracing()) std::fprintf(stderr, "[bink2w64] %s\n", std::format(fmt, std::forward<Args>(args)...).c_str());
}

[[noreturn]] void fatal(const std::string& what) {
    std::fprintf(stderr, "quillreel: bink2w64.dll: %s\n", what.c_str());
    std::abort();
}

thread_local DWORD t_last_error = 0;
void set_error(DWORD e) { t_last_error = e; }

std::atomic<DWORD> g_next_tid{0x100};
thread_local DWORD t_tid = 0;
DWORD current_tid() {
    if (!t_tid) t_tid = g_next_tid.fetch_add(4);
    return t_tid;
}

template <class Lock, class Pred>
bool wait_for(std::condition_variable& cv, Lock& lk, DWORD ms, Pred pred) {
    if (ms == kInfinite) {
        cv.wait(lk, pred);
        return true;
    }
    return cv.wait_for(lk, std::chrono::milliseconds(ms), pred);
}

std::string narrow(const WCHAR* s) {
    std::string out;
    if (s)
        for (; *s; ++s) out += *s < 0x80 ? static_cast<char>(*s) : '?';
    return out;
}

// ---- Kernel objects behind HANDLEs -----------------------------------------

struct Object {
    virtual ~Object() = default;
    virtual bool wait(DWORD) { return true; }  // true = signalled
};

struct File : Object {
    int fd = -1;
    bool owned = true;
    ~File() override {
        if (owned && fd >= 0) ::close(fd);
    }
};

struct Semaphore : Object {
    std::mutex m;
    std::condition_variable cv;
    LONG count = 0, max = 1;
    bool wait(DWORD ms) override {
        std::unique_lock lk(m);
        if (!wait_for(cv, lk, ms, [&] { return count > 0; })) return false;
        --count;
        return true;
    }
};

struct Mutex : Object {
    std::mutex m;
    std::condition_variable cv;
    DWORD owner = 0;
    int depth = 0;
    bool wait(DWORD ms) override {
        std::unique_lock lk(m);
        DWORD me = current_tid();
        if (!wait_for(cv, lk, ms, [&] { return depth == 0 || owner == me; })) return false;
        owner = me;
        ++depth;
        return true;
    }
};

struct Event : Object {
    std::mutex m;
    std::condition_variable cv;
    bool manual = false, signalled = false;
    bool wait(DWORD ms) override {
        std::unique_lock lk(m);
        if (!wait_for(cv, lk, ms, [&] { return signalled; })) return false;
        if (!manual) signalled = false;
        return true;
    }
};

struct Thread : Object {
    std::mutex m;
    std::condition_variable cv;
    bool suspended = false, done = false;
    DWORD exit_code = 259;  // STILL_ACTIVE
    DWORD id = 0;
    void* fn = nullptr;
    void* arg = nullptr;
    bool wait(DWORD ms) override {
        std::unique_lock lk(m);
        return wait_for(cv, lk, ms, [&] { return done; });
    }
};

class Handles {
public:
    HANDLE add(std::shared_ptr<Object> o) {
        std::lock_guard lk(m_);
        uintptr_t h = next_;
        next_ += 4;
        map_.emplace(h, std::move(o));
        return reinterpret_cast<HANDLE>(h);
    }
    std::shared_ptr<Object> get(HANDLE h) {
        std::lock_guard lk(m_);
        auto it = map_.find(reinterpret_cast<uintptr_t>(h));
        return it == map_.end() ? nullptr : it->second;
    }
    bool remove(HANDLE h) {
        std::shared_ptr<Object> keep;  // destroyed outside the lock
        std::lock_guard lk(m_);
        auto it = map_.find(reinterpret_cast<uintptr_t>(h));
        if (it == map_.end()) return false;
        keep = std::move(it->second);
        map_.erase(it);
        return true;
    }

private:
    std::mutex m_;
    std::unordered_map<uintptr_t, std::shared_ptr<Object>> map_;
    uintptr_t next_ = 0x1000;
};

// Never destroyed: the DLL's threads may still use handles during exit.
Handles& handles() {
    static Handles* h = new Handles;
    return *h;
}

template <class T>
std::shared_ptr<T> get_as(HANDLE h) {
    return std::dynamic_pointer_cast<T>(handles().get(h));
}

HANDLE std_handle(int fd) {
    static HANDLE hs[3] = {};
    static std::once_flag once;
    std::call_once(once, [] {
        for (int i = 0; i < 3; ++i) {
            auto f = std::make_shared<File>();
            f->fd = i;
            f->owned = false;
            hs[i] = handles().add(std::move(f));
        }
    });
    return hs[fd];
}

// ---- Modules ----------------------------------------------------------------

const pe::Image* g_image = nullptr;
alignas(16) char g_exe_module[16];  // GetModuleHandle(NULL)
alignas(16) char g_heap[16];        // the one heap handle

std::string module_key(std::string_view name) {
    std::string s(name.substr(name.find_last_of("\\/") == std::string_view::npos ? 0 : name.find_last_of("\\/") + 1));
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s.ends_with(".dll")) s.resize(s.size() - 4);
    return s;
}

bool known_module(const std::string& key) {
    static const char* const kKnown[] = {"kernel32", "kernelbase", "ntdll",  "user32",  "gdi32",
                                         "winmm",    "ole32",      "advapi32", "msvcrt", "shell32"};
    for (const char* k : kKnown)
        if (key == k) return true;
    return key.starts_with("api-ms-win-") || key.starts_with("ext-ms-");
}

// Each known system DLL gets a distinct fake handle (never dereferenced).
struct FakeModules {
    std::mutex m;
    std::map<std::string, std::unique_ptr<char[]>> by_name;
};
FakeModules& fake_modules() {
    static auto* f = new FakeModules;
    return *f;
}
HANDLE fake_module(const std::string& key) {
    FakeModules& f = fake_modules();
    std::lock_guard lk(f.m);
    auto& p = f.by_name[key];
    if (!p) p = std::make_unique<char[]>(16);
    return p.get();
}
bool is_fake_module(HANDLE h) {
    if (h == g_exe_module) return true;
    FakeModules& f = fake_modules();
    std::lock_guard lk(f.m);
    for (const auto& [name, p] : f.by_name)
        if (p.get() == h) return true;
    return false;
}


// ---- Heap -------------------------------------------------------------------

struct alignas(16) HeapHeader {
    size_t size;
    size_t pad;
};

void* heap_alloc(size_t n, bool zero) {
    auto* h = static_cast<HeapHeader*>(zero ? std::calloc(1, n + sizeof(HeapHeader)) : std::malloc(n + sizeof(HeapHeader)));
    if (!h) return nullptr;
    h->size = n;
    return h + 1;
}
void heap_free(void* p) {
    if (p) std::free(static_cast<HeapHeader*>(p) - 1);
}
size_t heap_size(const void* p) { return (static_cast<const HeapHeader*>(p) - 1)->size; }

// ---- The stand-ins ------------------------------------------------------------
// Signatures follow the Windows API; only what the C runtime and the decoder use
// is implemented faithfully.

DWORD WINAPI GetLastError() { return t_last_error; }
void WINAPI SetLastError(DWORD e) { t_last_error = e; }

HANDLE WINAPI HeapCreate(DWORD, size_t, size_t) { return g_heap; }
BOOL WINAPI HeapDestroy(HANDLE) { return 1; }
HANDLE WINAPI GetProcessHeap() { return g_heap; }
void* WINAPI HeapAlloc(HANDLE, DWORD flags, size_t n) {
    void* p = heap_alloc(n, flags & 8 /* HEAP_ZERO_MEMORY */);
    if (!p) set_error(kErrorNotEnoughMemory);
    return p;
}
void* WINAPI HeapReAlloc(HANDLE, DWORD flags, void* p, size_t n) {
    if (!p) return HeapAlloc(nullptr, flags, n);
    size_t old = heap_size(p);
    if ((flags & 0x10) /* HEAP_REALLOC_IN_PLACE_ONLY */ && n > old) {
        set_error(kErrorNotEnoughMemory);
        return nullptr;
    }
    auto* h = static_cast<HeapHeader*>(std::realloc(static_cast<HeapHeader*>(p) - 1, n + sizeof(HeapHeader)));
    if (!h) {
        set_error(kErrorNotEnoughMemory);
        return nullptr;
    }
    h->size = n;
    if ((flags & 8) && n > old) std::memset(reinterpret_cast<char*>(h + 1) + old, 0, n - old);
    return h + 1;
}
BOOL WINAPI HeapFree(HANDLE, DWORD, void* p) {
    heap_free(p);
    return 1;
}
size_t WINAPI HeapSize(HANDLE, DWORD, const void* p) { return p ? heap_size(p) : static_cast<size_t>(-1); }
BOOL WINAPI HeapValidate(HANDLE, DWORD, const void*) { return 1; }
BOOL WINAPI HeapSetInformation(HANDLE, int, void*, size_t) { return 1; }
void* WINAPI LocalAlloc(UINT flags, size_t n) { return heap_alloc(n, flags & 0x40 /* LMEM_ZEROINIT */); }
void* WINAPI LocalFree(void* p) {
    heap_free(p);
    return nullptr;
}

// CRITICAL_SECTION is 40 bytes of caller memory; its first word points to ours.
struct CritSec {
    std::recursive_mutex m;
};
CritSec*& cs_of(void* cs) { return *static_cast<CritSec**>(cs); }
void WINAPI InitializeCriticalSection(void* cs) {
    std::memset(cs, 0, 40);
    cs_of(cs) = new CritSec;
}
BOOL WINAPI InitializeCriticalSectionAndSpinCount(void* cs, DWORD) {
    InitializeCriticalSection(cs);
    return 1;
}
BOOL WINAPI InitializeCriticalSectionEx(void* cs, DWORD, DWORD) {
    InitializeCriticalSection(cs);
    return 1;
}
void WINAPI EnterCriticalSection(void* cs) { cs_of(cs)->m.lock(); }
BOOL WINAPI TryEnterCriticalSection(void* cs) { return cs_of(cs)->m.try_lock(); }
void WINAPI LeaveCriticalSection(void* cs) { cs_of(cs)->m.unlock(); }
void WINAPI DeleteCriticalSection(void* cs) {
    delete cs_of(cs);
    cs_of(cs) = nullptr;
}

// Fiber/thread local storage. Fiber callbacks are never run (no fibers here,
// and the C runtime only uses them to free per-thread data at thread exit).
constexpr DWORD kSlots = 256;
std::atomic<DWORD> g_next_slot{0};
thread_local void* t_slots[kSlots];
DWORD WINAPI FlsAlloc(void*) {
    DWORD i = g_next_slot.fetch_add(1);
    return i < kSlots ? i : 0xffffffff;
}
DWORD WINAPI TlsAlloc() { return FlsAlloc(nullptr); }
void* WINAPI FlsGetValue(DWORD i) {
    if (i >= kSlots) {
        set_error(kErrorInvalidParameter);
        return nullptr;
    }
    set_error(0);
    return t_slots[i];
}
BOOL WINAPI FlsSetValue(DWORD i, void* v) {
    if (i >= kSlots) {
        set_error(kErrorInvalidParameter);
        return 0;
    }
    t_slots[i] = v;
    return 1;
}
BOOL WINAPI FlsFree(DWORD) { return 1; }

void* WINAPI EncodePointer(void* p) { return p; }
void* WINAPI DecodePointer(void* p) { return p; }

int64_t mono_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000 + ts.tv_nsec;
}
void WINAPI GetSystemTimeAsFileTime(uint64_t* ft) {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    *ft = (static_cast<uint64_t>(ts.tv_sec) + 11644473600ull) * 10000000ull + static_cast<uint64_t>(ts.tv_nsec) / 100;
}
BOOL WINAPI QueryPerformanceCounter(int64_t* c) {
    *c = mono_ns() / 100;
    return 1;
}
BOOL WINAPI QueryPerformanceFrequency(int64_t* f) {
    *f = 10000000;
    return 1;
}
DWORD WINAPI GetTickCount() { return static_cast<DWORD>(mono_ns() / 1000000); }
uint64_t WINAPI GetTickCount64() { return static_cast<uint64_t>(mono_ns() / 1000000); }
DWORD WINAPI timeGetTime() { return GetTickCount(); }
UINT WINAPI timeBeginPeriod(UINT) { return 0; }
UINT WINAPI timeEndPeriod(UINT) { return 0; }
void WINAPI Sleep(DWORD ms) {
    if (ms == 0) {
        sched_yield();
        return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}
DWORD WINAPI SleepEx(DWORD ms, BOOL) {
    Sleep(ms);
    return 0;
}
BOOL WINAPI SwitchToThread() {
    sched_yield();
    return 1;
}

DWORD processor_count() { return std::clamp<DWORD>(std::thread::hardware_concurrency(), 1, 64); }
void WINAPI GetSystemInfo(uint8_t* si) {
    std::memset(si, 0, 48);
    auto put = [&](size_t off, auto v) { std::memcpy(si + off, &v, sizeof v); };
    put(0, uint16_t{9});  // PROCESSOR_ARCHITECTURE_AMD64
    put(4, DWORD{4096});
    put(8, uint64_t{0x10000});
    put(16, uint64_t{0x7ffffffeffff});
    DWORD n = processor_count();
    put(24, n >= 64 ? ~uint64_t{0} : (uint64_t{1} << n) - 1);
    put(32, n);
    put(36, DWORD{8664});
    put(40, DWORD{65536});
    put(44, uint16_t{6});
}
BOOL WINAPI IsProcessorFeaturePresent(DWORD f) {
    switch (f) {
    case 6: return __builtin_cpu_supports("sse");
    case 10: return __builtin_cpu_supports("sse2");
    case 13: return __builtin_cpu_supports("sse3");
    case 36: return __builtin_cpu_supports("ssse3");
    case 37: return __builtin_cpu_supports("sse4.1");
    case 38: return __builtin_cpu_supports("sse4.2");
    case 39: return __builtin_cpu_supports("avx");
    case 40: return __builtin_cpu_supports("avx2");
    default: return 0;
    }
}
DWORD WINAPI GetCurrentProcessId() { return static_cast<DWORD>(getpid()); }
HANDLE WINAPI GetCurrentProcess() { return kCurrentProcess; }
DWORD WINAPI GetCurrentThreadId() { return current_tid(); }
HANDLE WINAPI GetCurrentThread() { return kCurrentThread; }
BOOL WINAPI IsDebuggerPresent() { return 0; }

UINT WINAPI SetErrorMode(UINT) { return 0; }
void* WINAPI SetUnhandledExceptionFilter(void*) { return nullptr; }
LONG WINAPI UnhandledExceptionFilter(void*) { return 0; }
void* WINAPI AddVectoredExceptionHandler(DWORD, void*) { return g_heap; }
DWORD WINAPI RemoveVectoredExceptionHandler(void*) { return 1; }
void WINAPI ExitProcess(UINT code) { fatal(std::format("the library asked to exit the process (code {})", code)); }
BOOL WINAPI TerminateProcess(HANDLE, UINT code) {
    fatal(std::format("the library asked to terminate the process (code {})", code));
}
void WINAPI RaiseException(DWORD code, DWORD, DWORD, const uintptr_t*) {
    // 0x406D1388 names a thread for a debugger; without one it is caught by the
    // caller's own handler, which amounts to returning here.
    if (code == 0x406d1388) return;
    fatal(std::format("unhandled exception 0x{:08x}", code));
}
void WINAPI RtlUnwindEx(void*, void*, void*, void*, void*, void*) { fatal("exception unwinding is not supported"); }
void WINAPI RtlCaptureContext(void* ctx) { std::memset(ctx, 0, 1232); }
void* WINAPI RtlLookupFunctionEntry(uint64_t, uint64_t*, void*) { return nullptr; }
void* WINAPI RtlVirtualUnwind(DWORD, uint64_t, uint64_t, void*, void*, void**, uint64_t*, void*) { return nullptr; }
void WINAPI OutputDebugStringA(const char* s) { trace("debug output: {}", s ? s : ""); }
int WINAPI MessageBoxA(void*, const char* text, const char* caption, UINT) {
    std::fprintf(stderr, "quillreel: bink2w64.dll: %s: %s\n", caption ? caption : "message", text ? text : "");
    return 1;  // IDOK
}

DWORD WINAPI GetEnvironmentVariableA(const char*, char*, DWORD) {
    set_error(kErrorEnvvarNotFound);
    return 0;
}
DWORD WINAPI GetEnvironmentVariableW(const WCHAR*, WCHAR*, DWORD) {
    set_error(kErrorEnvvarNotFound);
    return 0;
}
char g_env_a[2] = {0, 0};
WCHAR g_env_w[2] = {0, 0};
char* WINAPI GetEnvironmentStrings() { return g_env_a; }
WCHAR* WINAPI GetEnvironmentStringsW() { return g_env_w; }
BOOL WINAPI FreeEnvironmentStringsA(char*) { return 1; }
BOOL WINAPI FreeEnvironmentStringsW(WCHAR*) { return 1; }
char g_cmdline_a[] = "quillreel.exe";
WCHAR g_cmdline_w[] = u"quillreel.exe";
char* WINAPI GetCommandLineA() { return g_cmdline_a; }
WCHAR* WINAPI GetCommandLineW() { return g_cmdline_w; }
void WINAPI GetStartupInfoA(void* si) {
    std::memset(si, 0, 104);
    *static_cast<DWORD*>(si) = 104;
}
void WINAPI GetStartupInfoW(void* si) { GetStartupInfoA(si); }
HANDLE WINAPI GetStdHandle(DWORD n) {
    switch (static_cast<int32_t>(n)) {
    case -10: return std_handle(0);
    case -11: return std_handle(1);
    case -12: return std_handle(2);
    default: set_error(kErrorInvalidHandle); return kInvalidHandle;
    }
}
UINT WINAPI SetHandleCount(UINT n) { return n; }

template <class Char>
DWORD put_string(std::string_view s, Char* buf, DWORD size) {
    if (!size) return static_cast<DWORD>(s.size()) + 1;
    DWORD n = std::min<DWORD>(static_cast<DWORD>(s.size()), size - 1);
    for (DWORD i = 0; i < n; ++i) buf[i] = static_cast<Char>(s[i]);
    buf[n] = 0;
    if (s.size() >= size) {
        set_error(kErrorInsufficientBuffer);
        return size;
    }
    return n;
}
std::string module_file_name(HANDLE mod) {
    return g_image && mod == g_image->base() ? "C:\\quillreel\\bink2w64.dll" : "C:\\quillreel\\quillreel.exe";
}
DWORD WINAPI GetModuleFileNameA(HANDLE mod, char* buf, DWORD size) { return put_string(module_file_name(mod), buf, size); }
DWORD WINAPI GetModuleFileNameW(HANDLE mod, WCHAR* buf, DWORD size) { return put_string(module_file_name(mod), buf, size); }
UINT system_dir(std::string_view s, char* buf, UINT size) {
    if (size > s.size()) {
        std::memcpy(buf, s.data(), s.size());
        buf[s.size()] = 0;
        return static_cast<UINT>(s.size());
    }
    return static_cast<UINT>(s.size()) + 1;
}
UINT WINAPI GetSystemDirectoryA(char* buf, UINT size) { return system_dir("C:\\Windows\\System32", buf, size); }
UINT WINAPI GetWindowsDirectoryA(char* buf, UINT size) { return system_dir("C:\\Windows", buf, size); }

// Code pages: everything is treated as single-byte Latin-1.
UINT WINAPI GetACP() { return 1252; }
UINT WINAPI GetOEMCP() { return 437; }
BOOL WINAPI IsValidCodePage(UINT) { return 1; }
BOOL WINAPI GetCPInfo(UINT, uint8_t* info) {
    std::memset(info, 0, 20);
    *reinterpret_cast<UINT*>(info) = 1;  // MaxCharSize
    info[4] = '?';                       // DefaultChar
    return 1;
}
int WINAPI GetLocaleInfoA(DWORD, DWORD, char*, int) {
    set_error(kErrorInvalidParameter);
    return 0;
}
int WINAPI GetLocaleInfoW(DWORD, DWORD, WCHAR*, int) {
    set_error(kErrorInvalidParameter);
    return 0;
}
int WINAPI MultiByteToWideChar(UINT, DWORD, const char* src, int srclen, WCHAR* dst, int dstlen) {
    if (srclen < 0) srclen = static_cast<int>(std::strlen(src)) + 1;
    if (dstlen == 0) return srclen;
    if (dstlen < srclen) {
        set_error(kErrorInsufficientBuffer);
        return 0;
    }
    for (int i = 0; i < srclen; ++i) dst[i] = static_cast<unsigned char>(src[i]);
    return srclen;
}
int WINAPI WideCharToMultiByte(UINT, DWORD, const WCHAR* src, int srclen, char* dst, int dstlen, const char* defchar,
                               BOOL* used_default) {
    if (srclen < 0) {
        srclen = 1;
        while (src[srclen - 1]) ++srclen;
    }
    if (used_default) *used_default = 0;
    if (dstlen == 0) return srclen;
    if (dstlen < srclen) {
        set_error(kErrorInsufficientBuffer);
        return 0;
    }
    for (int i = 0; i < srclen; ++i) {
        if (src[i] < 256) {
            dst[i] = static_cast<char>(src[i]);
        } else {
            dst[i] = defchar ? *defchar : '?';
            if (used_default) *used_default = 1;
        }
    }
    return srclen;
}
template <class Char>
int map_string(DWORD flags, const Char* src, int srclen, Char* dst, int dstlen) {
    if (srclen < 0) {
        srclen = 1;
        while (src[srclen - 1]) ++srclen;
    }
    if (dstlen == 0) return srclen;
    if (dstlen < srclen) {
        set_error(kErrorInsufficientBuffer);
        return 0;
    }
    for (int i = 0; i < srclen; ++i) {
        unsigned c = static_cast<unsigned>(src[i]);
        if (c < 128 && (flags & 0x100)) c = static_cast<unsigned>(std::tolower(static_cast<int>(c)));
        if (c < 128 && (flags & 0x200)) c = static_cast<unsigned>(std::toupper(static_cast<int>(c)));
        dst[i] = static_cast<Char>(c);
    }
    return srclen;
}
int WINAPI LCMapStringA(DWORD, DWORD flags, const char* src, int srclen, char* dst, int dstlen) {
    return map_string(flags, src, srclen, dst, dstlen);
}
int WINAPI LCMapStringW(DWORD, DWORD flags, const WCHAR* src, int srclen, WCHAR* dst, int dstlen) {
    return map_string(flags, src, srclen, dst, dstlen);
}
int WINAPI LCMapStringEx(const WCHAR*, DWORD flags, const WCHAR* src, int srclen, WCHAR* dst, int dstlen, void*,
                         void*, intptr_t) {
    return map_string(flags, src, srclen, dst, dstlen);
}
uint16_t ctype1(unsigned c) {
    if (c >= 128) return 0;
    int i = static_cast<int>(c);
    uint16_t t = 0x200;  // C1_DEFINED
    if (std::isupper(i)) t |= 0x1;
    if (std::islower(i)) t |= 0x2;
    if (std::isdigit(i)) t |= 0x4;
    if (std::isspace(i)) t |= 0x8;
    if (std::ispunct(i)) t |= 0x10;
    if (std::iscntrl(i)) t |= 0x20;
    if (c == ' ' || c == '\t') t |= 0x40;
    if (std::isxdigit(i)) t |= 0x80;
    if (std::isalpha(i)) t |= 0x100;
    return t;
}
BOOL WINAPI GetStringTypeA(DWORD, DWORD, const char* src, int n, uint16_t* out) {
    if (n < 0) n = static_cast<int>(std::strlen(src)) + 1;
    for (int i = 0; i < n; ++i) out[i] = ctype1(static_cast<unsigned char>(src[i]));
    return 1;
}
BOOL WINAPI GetStringTypeW(DWORD, const WCHAR* src, int n, uint16_t* out) {
    if (n < 0) {
        n = 1;
        while (src[n - 1]) ++n;
    }
    for (int i = 0; i < n; ++i) out[i] = ctype1(src[i]);
    return 1;
}

HANDLE module_handle(const char* name) {
    if (!name) return g_exe_module;
    std::string key = module_key(name);
    if (g_image && key == module_key(g_image->path())) return g_image->base();
    if (known_module(key)) return fake_module(key);
    set_error(kErrorModNotFound);
    return nullptr;
}
HANDLE WINAPI GetModuleHandleA(const char* name) { return module_handle(name); }
HANDLE WINAPI GetModuleHandleW(const WCHAR* name) { return name ? module_handle(narrow(name).c_str()) : g_exe_module; }
HANDLE load_library(const std::string& name) {
    HANDLE h = module_handle(name.c_str());
    if (!h) trace("LoadLibrary({}): not available", name);
    return h;
}
HANDLE WINAPI LoadLibraryA(const char* name) { return load_library(name ? name : ""); }
HANDLE WINAPI LoadLibraryW(const WCHAR* name) { return load_library(narrow(name)); }
HANDLE WINAPI LoadLibraryExA(const char* name, HANDLE, DWORD) { return load_library(name ? name : ""); }
HANDLE WINAPI LoadLibraryExW(const WCHAR* name, HANDLE, DWORD) { return load_library(narrow(name)); }
BOOL WINAPI FreeLibrary(HANDLE) { return 1; }
void* WINAPI GetProcAddress(HANDLE mod, const char* name);

int open_flags(DWORD access, DWORD disposition) {
    bool read = access & 0x80000000, write = access & 0x40000000;
    int flags = (read && write) ? O_RDWR : write ? O_WRONLY : O_RDONLY;
    switch (disposition) {
    case 1: flags |= O_CREAT | O_EXCL; break;   // CREATE_NEW
    case 2: flags |= O_CREAT | O_TRUNC; break;  // CREATE_ALWAYS
    case 4: flags |= O_CREAT; break;            // OPEN_ALWAYS
    case 5: flags |= O_TRUNC; break;            // TRUNCATE_EXISTING
    default: break;                             // OPEN_EXISTING
    }
    return flags | O_CLOEXEC;
}
HANDLE create_file(const std::string& name, DWORD access, DWORD disposition) {
    int fd = ::open(name.c_str(), open_flags(access, disposition), 0644);
    trace("CreateFile({}) -> {}", name, fd >= 0 ? "ok" : std::strerror(errno));
    if (fd < 0) {
        set_error(errno == ENOENT ? kErrorFileNotFound : 5 /* ERROR_ACCESS_DENIED */);
        return kInvalidHandle;
    }
    auto f = std::make_shared<File>();
    f->fd = fd;
    return handles().add(std::move(f));
}
HANDLE WINAPI CreateFileA(const char* name, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    return create_file(name ? name : "", access, disposition);
}
HANDLE WINAPI CreateFileW(const WCHAR* name, DWORD access, DWORD, void*, DWORD disposition, DWORD, HANDLE) {
    return create_file(narrow(name), access, disposition);
}
// OVERLAPPED: Internal, InternalHigh, Offset, OffsetHigh, hEvent. On a handle
// opened without FILE_FLAG_OVERLAPPED it just gives the position to use.
int64_t overlapped_offset(const void* ov) {
    uint32_t lo, hi;
    std::memcpy(&lo, static_cast<const char*>(ov) + 16, 4);
    std::memcpy(&hi, static_cast<const char*>(ov) + 20, 4);
    return static_cast<int64_t>((static_cast<uint64_t>(hi) << 32) | lo);
}
BOOL WINAPI ReadFile(HANDLE h, void* buf, DWORD n, DWORD* got, void* ov) {
    auto f = get_as<File>(h);
    if (got) *got = 0;
    if (!f) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    if (ov && ::lseek(f->fd, overlapped_offset(ov), SEEK_SET) < 0) {
        set_error(kErrorInvalidParameter);
        return 0;
    }
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::read(f->fd, static_cast<char*>(buf) + total, n - total);
        if (r < 0 && errno == EINTR) continue;
        if (r < 0) {
            set_error(30 /* ERROR_READ_FAULT */);
            return 0;
        }
        if (r == 0) break;
        total += static_cast<DWORD>(r);
    }
    if (got) *got = total;
    if (ov) {
        uint64_t zero = 0, done = total;
        std::memcpy(ov, &zero, 8);
        std::memcpy(static_cast<char*>(ov) + 8, &done, 8);
    }
    return 1;
}
BOOL WINAPI WriteFile(HANDLE h, const void* buf, DWORD n, DWORD* put, void* ov) {
    auto f = get_as<File>(h);
    if (put) *put = 0;
    if (!f) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    if (ov) ::lseek(f->fd, overlapped_offset(ov), SEEK_SET);
    DWORD total = 0;
    while (total < n) {
        ssize_t r = ::write(f->fd, static_cast<const char*>(buf) + total, n - total);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            set_error(29 /* ERROR_WRITE_FAULT */);
            return 0;
        }
        total += static_cast<DWORD>(r);
    }
    if (put) *put = total;
    return 1;
}
int whence(DWORD method) { return method == 1 ? SEEK_CUR : method == 2 ? SEEK_END : SEEK_SET; }
DWORD WINAPI SetFilePointer(HANDLE h, LONG lo, LONG* hi, DWORD method) {
    auto f = get_as<File>(h);
    if (!f) {
        set_error(kErrorInvalidHandle);
        return 0xffffffff;
    }
    int64_t dist = hi ? static_cast<int64_t>((static_cast<uint64_t>(static_cast<uint32_t>(*hi)) << 32) |
                                             static_cast<uint32_t>(lo))
                      : lo;
    off_t pos = ::lseek(f->fd, dist, whence(method));
    if (pos < 0) {
        set_error(131 /* ERROR_NEGATIVE_SEEK */);
        return 0xffffffff;
    }
    if (hi) *hi = static_cast<LONG>(static_cast<uint64_t>(pos) >> 32);
    set_error(0);
    return static_cast<DWORD>(pos);
}
BOOL WINAPI SetFilePointerEx(HANDLE h, int64_t dist, int64_t* newpos, DWORD method) {
    auto f = get_as<File>(h);
    if (!f) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    off_t pos = ::lseek(f->fd, dist, whence(method));
    if (pos < 0) {
        set_error(131);
        return 0;
    }
    if (newpos) *newpos = pos;
    return 1;
}
BOOL WINAPI GetFileSizeEx(HANDLE h, int64_t* size) {
    auto f = get_as<File>(h);
    struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    *size = st.st_size;
    return 1;
}
DWORD WINAPI GetFileSize(HANDLE h, DWORD* hi) {
    int64_t size = 0;
    if (!GetFileSizeEx(h, &size)) return 0xffffffff;
    if (hi) *hi = static_cast<DWORD>(static_cast<uint64_t>(size) >> 32);
    return static_cast<DWORD>(size);
}
DWORD WINAPI GetFileType(HANDLE h) {
    auto f = get_as<File>(h);
    struct stat st;
    if (!f || ::fstat(f->fd, &st) != 0) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    if (S_ISCHR(st.st_mode)) return 2;   // FILE_TYPE_CHAR
    if (S_ISFIFO(st.st_mode)) return 3;  // FILE_TYPE_PIPE
    return 1;                            // FILE_TYPE_DISK
}
DWORD WINAPI GetFileAttributesA(const char* name) {
    struct stat st;
    if (!name || ::stat(name, &st) != 0) {
        set_error(kErrorFileNotFound);
        return 0xffffffff;
    }
    return S_ISDIR(st.st_mode) ? 0x10 : 0x80;
}
BOOL WINAPI CloseHandle(HANDLE h) {
    if (h == kCurrentProcess || h == kCurrentThread) return 1;
    if (h == std_handle(0) || h == std_handle(1) || h == std_handle(2)) return 1;
    if (handles().remove(h)) return 1;
    set_error(kErrorInvalidHandle);
    return 0;
}

void* thread_main(void* p) {
    auto* holder = static_cast<std::shared_ptr<Thread>*>(p);
    std::shared_ptr<Thread> t = std::move(*holder);
    delete holder;
    t_tid = t->id;
    {
        std::unique_lock lk(t->m);
        t->cv.wait(lk, [&] { return !t->suspended; });
    }
    using Start = DWORD(WINAPI*)(void*);
    DWORD rc = reinterpret_cast<Start>(t->fn)(t->arg);
    {
        std::lock_guard lk(t->m);
        t->done = true;
        t->exit_code = rc;
    }
    t->cv.notify_all();
    return nullptr;
}
HANDLE WINAPI CreateThread(void*, size_t stack, void* fn, void* arg, DWORD flags, DWORD* tid) {
    auto t = std::make_shared<Thread>();
    t->fn = fn;
    t->arg = arg;
    t->suspended = flags & 4;  // CREATE_SUSPENDED
    t->id = g_next_tid.fetch_add(4);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, std::max<size_t>(stack, 8u << 20));
    auto* holder = new std::shared_ptr<Thread>(t);
    pthread_t th;
    int rc = pthread_create(&th, &attr, thread_main, holder);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        delete holder;
        set_error(kErrorNotEnoughMemory);
        return nullptr;
    }
    trace("CreateThread -> id {}", t->id);
    if (tid) *tid = t->id;
    return handles().add(std::move(t));
}
DWORD WINAPI ResumeThread(HANDLE h) {
    auto t = get_as<Thread>(h);
    if (!t) return 0xffffffff;
    DWORD prev;
    {
        std::lock_guard lk(t->m);
        prev = t->suspended ? 1 : 0;
        t->suspended = false;
    }
    t->cv.notify_all();
    return prev;
}
BOOL WINAPI GetExitCodeThread(HANDLE h, DWORD* code) {
    auto t = get_as<Thread>(h);
    if (!t) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    std::lock_guard lk(t->m);
    *code = t->exit_code;
    return 1;
}
BOOL WINAPI SetThreadPriority(HANDLE, int) { return 1; }
int WINAPI GetThreadPriority(HANDLE) { return 0; }
uintptr_t WINAPI SetThreadAffinityMask(HANDLE, uintptr_t) { return 1; }
HANDLE WINAPI OpenThread(DWORD, BOOL, DWORD) { return handles().add(std::make_shared<Object>()); }

HANDLE make_semaphore(LONG initial, LONG max) {
    if (max <= 0 || initial < 0 || initial > max) {
        set_error(kErrorInvalidParameter);
        return nullptr;
    }
    auto s = std::make_shared<Semaphore>();
    s->count = initial;
    s->max = max;
    return handles().add(std::move(s));
}
HANDLE WINAPI CreateSemaphoreA(void*, LONG initial, LONG max, const char*) { return make_semaphore(initial, max); }
HANDLE WINAPI CreateSemaphoreW(void*, LONG initial, LONG max, const WCHAR*) { return make_semaphore(initial, max); }
HANDLE WINAPI CreateSemaphoreExW(void*, LONG initial, LONG max, const WCHAR*, DWORD, DWORD) {
    return make_semaphore(initial, max);
}
BOOL WINAPI ReleaseSemaphore(HANDLE h, LONG n, LONG* prev) {
    auto s = get_as<Semaphore>(h);
    if (!s || n <= 0) {
        set_error(s ? kErrorInvalidParameter : kErrorInvalidHandle);
        return 0;
    }
    {
        std::lock_guard lk(s->m);
        if (s->count > s->max - n) {
            set_error(298 /* ERROR_TOO_MANY_POSTS */);
            return 0;
        }
        if (prev) *prev = s->count;
        s->count += n;
    }
    s->cv.notify_all();
    return 1;
}
HANDLE make_mutex(BOOL owned) {
    auto m = std::make_shared<Mutex>();
    if (owned) {
        m->owner = current_tid();
        m->depth = 1;
    }
    return handles().add(std::move(m));
}
HANDLE WINAPI CreateMutexA(void*, BOOL owned, const char*) { return make_mutex(owned); }
HANDLE WINAPI CreateMutexW(void*, BOOL owned, const WCHAR*) { return make_mutex(owned); }
BOOL WINAPI ReleaseMutex(HANDLE h) {
    auto m = get_as<Mutex>(h);
    if (!m) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    {
        std::lock_guard lk(m->m);
        if (m->depth == 0 || m->owner != current_tid()) {
            set_error(288 /* ERROR_NOT_OWNER */);
            return 0;
        }
        --m->depth;
    }
    m->cv.notify_all();
    return 1;
}
HANDLE make_event(BOOL manual, BOOL initial) {
    auto e = std::make_shared<Event>();
    e->manual = manual;
    e->signalled = initial;
    return handles().add(std::move(e));
}
HANDLE WINAPI CreateEventA(void*, BOOL manual, BOOL initial, const char*) { return make_event(manual, initial); }
HANDLE WINAPI CreateEventW(void*, BOOL manual, BOOL initial, const WCHAR*) { return make_event(manual, initial); }
BOOL set_event(HANDLE h, bool on) {
    auto e = get_as<Event>(h);
    if (!e) {
        set_error(kErrorInvalidHandle);
        return 0;
    }
    {
        std::lock_guard lk(e->m);
        e->signalled = on;
    }
    if (on) e->cv.notify_all();
    return 1;
}
BOOL WINAPI SetEvent(HANDLE h) { return set_event(h, true); }
BOOL WINAPI ResetEvent(HANDLE h) { return set_event(h, false); }
DWORD WINAPI WaitForSingleObject(HANDLE h, DWORD ms) {
    auto o = handles().get(h);
    if (!o) {
        set_error(kErrorInvalidHandle);
        return kWaitFailed;
    }
    return o->wait(ms) ? 0 : kWaitTimeout;
}
DWORD WINAPI WaitForSingleObjectEx(HANDLE h, DWORD ms, BOOL) { return WaitForSingleObject(h, ms); }

// Sound and COM: only used for audio output, which Quillreel does itself.
UINT WINAPI waveOutOpen(void*, UINT, void*, uintptr_t, uintptr_t, DWORD) { return 6; /* MMSYSERR_NODRIVER */ }
void WINAPI CoTaskMemFree(void* p) { (void)p; }
LONG WINAPI PropVariantClear(void*) { return 0; }

#define F(name) {#name, reinterpret_cast<void*>(&name)}
const std::unordered_map<std::string_view, void*>& functions() {
    static const std::unordered_map<std::string_view, void*> table = {
        F(GetLastError), F(SetLastError), F(HeapCreate), F(HeapDestroy), F(GetProcessHeap), F(HeapAlloc),
        F(HeapReAlloc), F(HeapFree), F(HeapSize), F(HeapValidate), F(HeapSetInformation), F(LocalAlloc),
        F(LocalFree), F(InitializeCriticalSection), F(InitializeCriticalSectionAndSpinCount),
        F(InitializeCriticalSectionEx), F(EnterCriticalSection), F(TryEnterCriticalSection),
        F(LeaveCriticalSection), F(DeleteCriticalSection), F(FlsAlloc), F(FlsGetValue), F(FlsSetValue), F(FlsFree),
        {"TlsAlloc", reinterpret_cast<void*>(&TlsAlloc)}, {"TlsGetValue", reinterpret_cast<void*>(&FlsGetValue)},
        {"TlsSetValue", reinterpret_cast<void*>(&FlsSetValue)}, {"TlsFree", reinterpret_cast<void*>(&FlsFree)},
        F(EncodePointer), F(DecodePointer), F(GetSystemTimeAsFileTime), F(QueryPerformanceCounter),
        F(QueryPerformanceFrequency), F(GetTickCount), F(GetTickCount64), F(timeGetTime), F(timeBeginPeriod),
        F(timeEndPeriod), F(Sleep), F(SleepEx), F(SwitchToThread), F(GetSystemInfo),
        {"GetNativeSystemInfo", reinterpret_cast<void*>(&GetSystemInfo)}, F(IsProcessorFeaturePresent),
        F(GetCurrentProcessId), F(GetCurrentProcess), F(GetCurrentThreadId), F(GetCurrentThread),
        F(IsDebuggerPresent), F(SetErrorMode), F(SetUnhandledExceptionFilter), F(UnhandledExceptionFilter),
        F(AddVectoredExceptionHandler), F(RemoveVectoredExceptionHandler), F(ExitProcess), F(TerminateProcess),
        F(RaiseException), F(RtlUnwindEx), F(RtlCaptureContext), F(RtlLookupFunctionEntry), F(RtlVirtualUnwind),
        F(OutputDebugStringA), F(MessageBoxA), F(GetEnvironmentVariableA), F(GetEnvironmentVariableW),
        F(GetEnvironmentStrings), F(GetEnvironmentStringsW), F(FreeEnvironmentStringsA),
        F(FreeEnvironmentStringsW), F(GetCommandLineA), F(GetCommandLineW), F(GetStartupInfoA),
        F(GetStartupInfoW), F(GetStdHandle), F(SetHandleCount), F(GetModuleFileNameA), F(GetModuleFileNameW),
        F(GetSystemDirectoryA), F(GetWindowsDirectoryA), F(GetACP), F(GetOEMCP), F(IsValidCodePage), F(GetCPInfo),
        F(GetLocaleInfoA), F(GetLocaleInfoW), F(MultiByteToWideChar), F(WideCharToMultiByte), F(LCMapStringA),
        F(LCMapStringW), F(LCMapStringEx), F(GetStringTypeA), F(GetStringTypeW), F(GetModuleHandleA),
        F(GetModuleHandleW), F(LoadLibraryA), F(LoadLibraryW), F(LoadLibraryExA), F(LoadLibraryExW),
        F(FreeLibrary), F(GetProcAddress), F(CreateFileA), F(CreateFileW), F(ReadFile), F(WriteFile),
        F(SetFilePointer), F(SetFilePointerEx), F(GetFileSizeEx), F(GetFileSize), F(GetFileType),
        F(GetFileAttributesA), F(CloseHandle), F(CreateThread), F(ResumeThread), F(GetExitCodeThread),
        F(SetThreadPriority), F(GetThreadPriority), F(SetThreadAffinityMask), F(OpenThread), F(CreateSemaphoreA),
        F(CreateSemaphoreW), F(CreateSemaphoreExW), F(ReleaseSemaphore), F(CreateMutexA), F(CreateMutexW),
        F(ReleaseMutex), F(CreateEventA), F(CreateEventW), F(SetEvent), F(ResetEvent), F(WaitForSingleObject),
        F(WaitForSingleObjectEx), F(waveOutOpen), F(CoTaskMemFree), F(PropVariantClear),
    };
    return table;
}
#undef F

void* WINAPI GetProcAddress(HANDLE mod, const char* name) {
    if (reinterpret_cast<uintptr_t>(name) < 0x10000) {  // by ordinal
        set_error(kErrorProcNotFound);
        return nullptr;
    }
    if (g_image && mod == g_image->base()) return g_image->export_address(name);
    if (mod && is_fake_module(mod)) {
        auto it = functions().find(name);
        if (it != functions().end()) return it->second;
    }
    trace("GetProcAddress({}): not available", name);
    set_error(kErrorProcNotFound);
    return nullptr;
}

// ---- Stubs for imports without a stand-in --------------------------------------

uint64_t WINAPI unsupported_call(const char* name) {
    trace("call to unsupported {} (returning 0)", name);
    set_error(kErrorCallNotImplemented);
    return 0;
}

// Each stub loads its name into the first argument register and jumps to
// unsupported_call:  mov rcx, imm64; mov rax, imm64; jmp rax
class StubPool {
public:
    void* make(std::string name) {
        static constexpr size_t kStub = 32, kPool = 64 * 1024;
        if (!mem_) {
            void* m = mmap(nullptr, kPool, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (m == MAP_FAILED) fatal("out of memory");
            mem_ = static_cast<uint8_t*>(m);
            cap_ = kPool;
        }
        if (sealed_ || used_ + kStub > cap_) fatal("too many unsupported imports");
        const char* s = names_.emplace_back(std::move(name)).c_str();
        auto fn = reinterpret_cast<uint64_t>(&unsupported_call);
        uint8_t* p = mem_ + used_;
        p[0] = 0x48, p[1] = 0xb9;
        std::memcpy(p + 2, &s, 8);
        p[10] = 0x48, p[11] = 0xb8;
        std::memcpy(p + 12, &fn, 8);
        p[20] = 0xff, p[21] = 0xe0;
        used_ += kStub;
        return p;
    }
    void seal() {
        if (mem_ && !sealed_) mprotect(mem_, cap_, PROT_READ | PROT_EXEC);
        sealed_ = true;
    }

private:
    uint8_t* mem_ = nullptr;
    size_t used_ = 0, cap_ = 0;
    bool sealed_ = false;
    std::deque<std::string> names_;  // stable addresses
};

StubPool& stubs() {
    static StubPool* p = new StubPool;
    return *p;
}

}  // namespace

void* resolve_import(std::string_view dll, std::string_view name) {
    if (name.starts_with("module:")) {
        std::string key = module_key(name.substr(7));
        return fake_module(key);
    }
    auto it = functions().find(name);
    if (it != functions().end()) return it->second;
    return stubs().make(std::format("{}!{}", dll, name));
}

void finish_load(const pe::Image& image) {
    g_image = &image;
    stubs().seal();
}

}  // namespace quillreel::win32
