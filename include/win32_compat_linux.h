// Minimal Win32 compatibility shim for the Linux Vulkan port.
//
// The upstream code is written against Windows headers (<Windows.h>, <d3d12.h>,
// <winrt/base.h>). For the Linux port we replace the *rendering* layer with
// pure Vulkan (no D3D12), but the rest of the codebase still uses a handful of
// Win32 types (HANDLE for events, BOOL, DWORD, etc.) and helper functions that
// don't have a clean equivalent. This header provides the minimum set so the
// gameplay/hooking code compiles unchanged.
//
// Anything that genuinely needs Win32 semantics (MessageBox UI, registry,
// D3D12) is either no-op'd or sentinel-valued; the only places this matters on
// Linux are fatal-error paths where the program is about to die anyway.
#pragma once

#if BETTERVR_HAS_WIN32
    #error "win32_compat_linux.h must not be included on Windows builds"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <time.h>

// ---- Win32 type aliases (opaque pointers on Linux) ----
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
using UINT    = unsigned int;
using ULONG   = unsigned long;
using LONG    = long;

// ---- LUID (used by upstream's D3D12 LUID adapter matching). On Linux we use
// VkPhysicalDevice directly from xrGetVulkanGraphicsDevice2KHR, but the type
// still appears in OpenXR::Capabilities, so keep it as a stub. ----
struct LUID {
    uint32_t LowPart;
    int32_t  HighPart;
};

// ---- D3D feature level — sentinel only; never queried on Linux.
// All values declared because logger.h's formatter switches on them. ----
enum D3D_FEATURE_LEVEL : int {
    D3D_FEATURE_LEVEL_1_0_CORE = 0x1000,
    D3D_FEATURE_LEVEL_9_1   = 0x9100,
    D3D_FEATURE_LEVEL_9_2   = 0x9200,
    D3D_FEATURE_LEVEL_9_3   = 0x9300,
    D3D_FEATURE_LEVEL_10_0  = 0xA000,
    D3D_FEATURE_LEVEL_10_1  = 0xA100,
    D3D_FEATURE_LEVEL_11_0  = 0xB000,
    D3D_FEATURE_LEVEL_11_1  = 0xB100,
    D3D_FEATURE_LEVEL_12_0  = 0xC000,
    D3D_FEATURE_LEVEL_12_1  = 0xC100,
};

// ---- DXGI_FORMAT — stub enum so the logger's std::formatter compiles. Real
// DXGI_FORMAT-bound code paths (D3D12 swapchain creation) are gone on Linux;
// the rendering layer uses VkFormat exclusively. Only the values that appear
// in logger.h's formatter switch + a handful of upstream Layer3D/Layer2D
// references need to exist as enumerators. ----
enum DXGI_FORMAT : int {
    DXGI_FORMAT_UNKNOWN              = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB  = 29,
    DXGI_FORMAT_B8G8R8A8_UNORM_SRGB  = 91,
    DXGI_FORMAT_D32_FLOAT            = 40,
    DXGI_FORMAT_D16_UNORM            = 55,
    DXGI_FORMAT_R32G32B32_TYPELESS   = 5,
    DXGI_FORMAT_D24_UNORM_S8_UINT    = 45,
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
};

// ---- HRESULT (return values from a few helpers) ----
using HRESULT = int32_t;
constexpr HRESULT S_OK = 0;
inline constexpr bool FAILED(HRESULT hr) { return hr < 0; }
inline constexpr bool SUCCEEDED(HRESULT hr) { return hr >= 0; }

// ---- LARGE_INTEGER + QueryPerformance{Frequency,Counter} (used in profiler) -
struct LARGE_INTEGER { int64_t QuadPart; };
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* li) {
    li->QuadPart = 1'000'000'000;
    return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* li) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    li->QuadPart = static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
    return 1;
}

// ---- MessageBoxA — log to stderr; callers always abort afterwards ----
constexpr unsigned MB_OK         = 0;
constexpr unsigned MB_ICONERROR  = 0;
constexpr unsigned MB_ICONWARNING= 0;
inline int MessageBoxA(HWND, const char* text, const char* caption, unsigned) {
    std::fprintf(stderr, "[BetterVR][%s] %s\n",
                 caption ? caption : "Error",
                 text    ? text    : "");
    return 0;
}

// ---- __debugbreak — gdb-friendly trap ----
#if defined(__has_builtin) && __has_builtin(__builtin_debugtrap)
    #define __debugbreak() __builtin_debugtrap()
#else
    #include <signal.h>
    #define __debugbreak() raise(SIGTRAP)
#endif

// ---- Standard-handle IDs used by Log::print's WriteConsoleA path ----
constexpr DWORD STD_OUTPUT_HANDLE = (DWORD)-11;
constexpr DWORD STD_ERROR_HANDLE  = (DWORD)-12;
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
inline BOOL AllocConsole() { return 1; }
inline BOOL FreeConsole()  { return 1; }
inline BOOL SetConsoleTitleA(const char*) { return 1; }

// ---- Case-insensitive compare ----
#define stricmp  strcasecmp
#define _stricmp strcasecmp

// ---- strcpy_s / strncpy_s (Microsoft safe-string fns). The bounded forms map
// cleanly to standard strncpy. The 4-arg variants on Windows take
// (dst, dstSize, src, count) — for our use the 3-arg forms work, but we
// implement the 4-arg with an extra dst-size check just in case. ----
inline int strncpy_s(char* dst, size_t dstSize, const char* src, size_t count) {
    if (!dst || !src || dstSize == 0) return 22; // EINVAL-ish
    size_t copy = (count < dstSize) ? count : (dstSize - 1);
    std::strncpy(dst, src, copy);
    dst[copy] = '\0';
    return 0;
}
inline int strcpy_s(char* dst, size_t dstSize, const char* src) {
    if (!dst || !src || dstSize == 0) return 22;
    std::strncpy(dst, src, dstSize - 1);
    dst[dstSize - 1] = '\0';
    return 0;
}
// MSVC has implicit array-size template overloads for strncpy_s / strcpy_s.
// Provide the equivalents so callers like `strcpy_s(actionSetInfo.actionSetName, "foo")`
// (with a char array dst) compile unchanged.
template <size_t N>
inline int strncpy_s(char (&dst)[N], const char* src, size_t count) {
    return strncpy_s(dst, N, src, count);
}
template <size_t N>
inline int strcpy_s(char (&dst)[N], const char* src) {
    return strcpy_s(dst, N, src);
}

// ---- WM_CLOSE: Win32 message constant; on Linux this is dead code on the
// session exit path, so we just provide the constant. ----
constexpr unsigned WM_CLOSE = 0x0010;
// PostMessage is used to signal Cemu to close on OpenXR session exit. On
// Linux we don't have a window message queue; stub to no-op (the OpenXR
// session destroy path still runs).
inline BOOL PostMessage(HWND, unsigned, uintptr_t, intptr_t) { return 1; }

// ---- Dynamic library helpers (Win32 LoadLibrary family). On Linux these
// map to dlopen/dlsym/dlclose. The layer uses these to find Cemu's exported
// HLE functions (`gameMeta_getTitleId`, `memory_getBase`, `osLib_registerHLEFunction`)
// from inside Cemu's address space — passing NULL as the module gives the
// loader's whole search list (RTLD_DEFAULT). ----
#include <dlfcn.h>
// Sentinel returned for GetModuleHandleA(NULL). RTLD_DEFAULT is literally
// (void*)0 on glibc, which would collide with upstream's `handle != NULL`
// checks. Use dlopen(NULL, ...) instead — it returns a non-NULL handle to
// the main executable.
inline HMODULE GetModuleHandleA(const char* name) {
    if (!name || !*name) {
        // Win32 NULL → calling process. Linux equivalent: dlopen(NULL).
        // RTLD_NOLOAD is fine here — the main exe is always loaded.
        return (HMODULE)dlopen(NULL, RTLD_NOW | RTLD_NOLOAD);
    }
    return (HMODULE)dlopen(name, RTLD_NOW | RTLD_NOLOAD);
}
inline void* GetProcAddress(HMODULE mod, const char* name) {
    return dlsym(mod ? mod : RTLD_DEFAULT, name);
}
inline BOOL FreeLibrary(HMODULE mod) {
    if (!mod) return 1;
    return dlclose(mod) == 0 ? 1 : 0;
}

// ---- CPUID — zero out (logger displays brand string, empty is OK) ----
inline void __cpuid(int out[4], int /*leaf*/) { out[0]=out[1]=out[2]=out[3]=0; }

// ---- RAM info via /proc/meminfo (called once at startup by logger) ----
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

// ---- UTF-16 → UTF-8 (logger uses this for adapter descriptions; on Linux we
// only ever get ASCII from Vulkan's deviceName, so a passthrough copy works) -
constexpr unsigned CP_UTF8 = 65001;
inline int WideCharToMultiByte(unsigned /*codepage*/, DWORD /*flags*/,
                               const wchar_t* wstr, int wlen,
                               char* out, int outLen,
                               const char* /*default*/, int* /*usedDefault*/) {
    if (!wstr) return 0;
    // Treat input as already-ASCII; copy with truncation.
    int written = 0;
    for (int i = 0; (wlen < 0 || i < wlen); ++i) {
        wchar_t c = wstr[i];
        if (out && written < outLen) out[written] = static_cast<char>(c & 0xFF);
        ++written;
        if (c == 0) break;
    }
    return written;
}

// ---- ComPtr stub. Upstream uses Microsoft::WRL::ComPtr<ID3D12X> heavily; on
// Linux we delete all D3D12 references. But pch.h still does
// `using Microsoft::WRL::ComPtr;` for the few places where the type appears in
// non-rendering code. A minimal smart-pointer wrapper that holds a raw pointer
// is enough — nothing on Linux should actually instantiate one with a real
// COM type. ----
namespace Microsoft::WRL {
    template <typename T>
    class ComPtr {
    public:
        ComPtr() = default;
        ComPtr(T* p) : m_ptr(p) {}
        ~ComPtr() = default;
        T* Get() const { return m_ptr; }
        T* operator->() const { return m_ptr; }
        T** GetAddressOf() { return &m_ptr; }
        T** ReleaseAndGetAddressOf() { m_ptr = nullptr; return &m_ptr; }
        explicit operator bool() const { return m_ptr != nullptr; }
    private:
        T* m_ptr = nullptr;
    };
}
