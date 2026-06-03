// Minimal Win32 compatibility shim for the Linux port.
// Only types/functions actually referenced by the non-launcher source are stubbed.
// Anything that requires real semantics (D3D12, registry, MessageBox UI) is a
// no-op or a sentinel value — the caller paths are gated by BETTERVR_HAS_D3D12
// or only fire on fatal-error paths where stderr is acceptable.
#pragma once

#if BETTERVR_HAS_WIN32
    #error "win32_compat_linux.h must not be included on Windows builds"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>

// Handle / module types — opaque pointers on Linux
using HANDLE = void*;
using HMODULE = void*;
using HWND    = void*;
using HKEY    = void*;
using BOOL    = int;
using DWORD   = uint32_t;
using LPVOID  = void*;
using LPCVOID = const void*;
using LPCSTR  = const char*;
using LPCWSTR = const wchar_t*;

// Result codes
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
inline constexpr bool FAILED(HRESULT hr) { return hr < 0; }
inline constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }

// Timing
struct LARGE_INTEGER {
    int64_t QuadPart;
};

// MessageBox flags — kept as ints to compile call sites unchanged
constexpr unsigned MB_OK         = 0;
constexpr unsigned MB_ICONERROR  = 0;
constexpr unsigned MB_ICONWARNING= 0;

// MessageBoxA: on Linux just log to stderr and continue.
// All current call sites in src/ (not launcher/) only fire on fatal errors
// that subsequently throw std::runtime_error, so we lose nothing meaningful.
inline int MessageBoxA(HWND, const char* text, const char* caption, unsigned) {
    std::fprintf(stderr, "[BetterVR][%s] %s\n",
                 caption ? caption : "Error",
                 text    ? text    : "");
    return 0;
}

// __debugbreak: SIGTRAP on Linux gives gdb a clean stop
#if defined(__has_builtin)
    #if __has_builtin(__builtin_debugtrap)
        #define __debugbreak() __builtin_debugtrap()
    #else
        #include <signal.h>
        #define __debugbreak() raise(SIGTRAP)
    #endif
#else
    #include <signal.h>
    #define __debugbreak() raise(SIGTRAP)
#endif

// Standard handle IDs — only consoleHandle in logger.cpp uses them, and we'll
// gate that call site
constexpr DWORD STD_OUTPUT_HANDLE = (DWORD)-11;
constexpr DWORD STD_ERROR_HANDLE  = (DWORD)-12;

// Win32 console / debug helpers used by Log::print. Map to stdout/stderr.
inline HANDLE GetStdHandle(DWORD which) {
    return (which == STD_ERROR_HANDLE) ? (HANDLE)stderr : (HANDLE)stdout;
}
inline BOOL WriteConsoleA(HANDLE h, const void* buf, DWORD len, DWORD* written, void*) {
    std::FILE* f = (h == (HANDLE)stderr) ? stderr : stdout;
    size_t n = std::fwrite(buf, 1, len, f);
    if (written) *written = static_cast<DWORD>(n);
    return n == len ? 1 : 0;
}
inline void OutputDebugStringA(const char* s) { std::fputs(s, stderr); }

// Console allocation — no-ops on Linux (the layer logs to stderr / file)
inline BOOL AllocConsole() { return 1; }
inline BOOL FreeConsole()  { return 1; }
inline BOOL SetConsoleTitleA(const char*) { return 1; }

// Case-insensitive compare lives in <strings.h> on POSIX
#include <strings.h>
#define stricmp  strcasecmp
#define _stricmp strcasecmp

// CPUID — accept the array form clang's <cpuid.h> doesn't expose directly.
// We just zero the output; logger.cpp uses it only for the displayed CPU brand
// string and gracefully prints nothing when the brand is empty.
inline void __cpuid(int out[4], int /*leaf*/) { out[0]=out[1]=out[2]=out[3]=0; }

// RAM info — fill with /proc/meminfo (cheap, called once at startup)
struct MEMORYSTATUSEX {
    DWORD    dwLength;
    DWORD    dwMemoryLoad;
    uint64_t ullTotalPhys;
    uint64_t ullAvailPhys;
    uint64_t ullTotalPageFile;
    uint64_t ullAvailPageFile;
    uint64_t ullTotalVirtual;
    uint64_t ullAvailVirtual;
    uint64_t ullAvailExtendedVirtual;
};
inline BOOL GlobalMemoryStatusEx(MEMORYSTATUSEX* s) {
    if (!s) return 0;
    std::FILE* f = std::fopen("/proc/meminfo", "r");
    if (!f) return 0;
    char key[64];
    uint64_t kb = 0;
    while (std::fscanf(f, "%63s %lu kB\n", key, &kb) == 2) {
        if (std::strcmp(key, "MemTotal:") == 0) {
            s->ullTotalPhys = kb * 1024ULL;
            break;
        }
    }
    std::fclose(f);
    return 1;
}

// High-resolution timing — wrap clock_gettime
#include <time.h>
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* li) {
    li->QuadPart = 1'000'000'000; // nanoseconds
    return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* li) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    li->QuadPart = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    return 1;
}

#include <cstring> // strcmp used above
