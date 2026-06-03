#pragma once

#include <atomic>
#include <string>
#include <variant>
#include <functional>
#include <type_traits>
#include <ranges>
#include <set>
#include <unordered_set>
#include <queue>
#include <iostream>
#include <vector>
#include <algorithm>
#include <thread>
#include <cstring>
#include <thread>
#include <chrono>
#include <algorithm>
#include <cctype>

#if defined(_WIN32)
    #define BETTERVR_HAS_D3D12 1
    #define BETTERVR_HAS_WIN32 1
#else
    #define BETTERVR_HAS_D3D12 0
    #define BETTERVR_HAS_WIN32 0
#endif

#if BETTERVR_HAS_WIN32
    #include <Windows.h>
    #include <winrt/base.h>
    #include <shellapi.h>

    // These macros mess with some of Vulkan's functions
    #undef ERROR
    #undef CreateEvent
    #undef CreateSemaphore

    #define VK_USE_PLATFORM_WIN32_KHR
#else
    #include <dlfcn.h>
    #include <cstdint>
    #include "win32_compat_linux.h"
#endif

#define VK_NO_PROTOTYPES
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

// vkroots vulkan layer framework includes
#define VKROOTS_NEGOTIATION_INTERFACE VRLayer_NegotiateLoaderLayerInterfaceVersion
#include "vkroots.h"

#if BETTERVR_HAS_D3D12
    // D3D12 includes
    #include <d3d12.h>
    #include <D3Dcompiler.h>
    #include <dxgi1_6.h>

    #pragma comment(lib, "d3d12.lib")
    #pragma comment(lib, "dxgi.lib")
    #pragma comment(lib, "D3DCompiler.lib")
    #pragma comment(lib, "dxguid.lib")

    #include <wrl/client.h>

    using Microsoft::WRL::ComPtr;
#endif

// OpenXR includes
#if BETTERVR_HAS_WIN32
    #define XR_USE_PLATFORM_WIN32
#endif
#if BETTERVR_HAS_D3D12
    #define XR_USE_GRAPHICS_API_D3D12
#else
    #define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

// ImGui includes
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <implot.h>
#include <imgui_memory_editor.h>

// glm includes
#define GLM_FORCE_XYZW_ONLY
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_access.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/norm.hpp>
#include <glm/gtx/string_cast.hpp>
#include <glm/gtx/euler_angles.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#define ENABLE_VK_ROBUSTNESS 0

#include "font_kenney.h"


inline glm::fvec2 ToGLM(const XrVector2f& vec) {
    return glm::make_vec2(&vec.x);
}

inline glm::fvec3 ToGLM(const XrVector3f& vec) {
    return glm::make_vec3(&vec.x);
}

inline glm::fquat ToGLM(const XrQuaternionf& quat) {
    return glm::fquat(quat.w, quat.x, quat.y, quat.z);
}


inline XrVector2f ToXR(const glm::fvec2& vec) {
    return { vec.x, vec.y };
}

inline XrVector3f ToXR(const glm::fvec3& vec) {
    return { vec.x, vec.y, vec.z };
}

inline XrQuaternionf ToXR(const glm::fquat& quat) {
    return { quat.x, quat.y, quat.z, quat.w };
}

inline glm::fmat4 ToMat4(const glm::fvec3& pos) {
    return glm::translate(glm::identity<glm::fmat4>(), pos);
}

inline glm::fmat4 ToMat4(const glm::fquat& rot) {
    return glm::mat4(rot);
}

inline glm::fmat4 ToMat4(const glm::fvec3& pos, const glm::fquat& rot) {
    return ToMat4(pos) * ToMat4(rot);
}


inline std::string toLower(std::string str) {
    std::ranges::transform(str, str.begin(), [](unsigned char c) { return std::tolower(c); });
    return str;
}

inline uint32_t stringToHash(const char* str) {
    uint32_t hash = 0;
    while (*str) {
        hash = (hash << 7) + *str++;
    }
    return hash;
}

inline std::string wcharToUtf8(const wchar_t* wstr) {
#if BETTERVR_HAS_WIN32
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr, -1, nullptr, 0, nullptr, nullptr);
    std::string str(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr, -1, &str[0], size_needed, nullptr, nullptr);
    return str;
#else
    // Minimal portable conversion (assumes wchar_t holds UCS-4 / UCS-2 on Linux)
    std::string out;
    if (!wstr) return out;
    for (; *wstr; ++wstr) {
        wchar_t c = *wstr;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (c >> 18));
            out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
#endif
}

// Token-paste only between byte_ and the hex literal — the brackets stay as
// regular tokens. MSVC tolerated the extra `##` non-standardly; Clang doesn't.
#define PADDED_BYTES(from, up) uint8_t byte_##from[(up-from+0x04)]

template<class T, template<class...> class U>
inline constexpr bool is_instance_of_v = std::false_type{};

template<template<class...> class U, class... Vs>
inline constexpr bool is_instance_of_v<U<Vs...>,U> = std::true_type{};

template <typename T>
requires std::is_enum_v<T>
constexpr bool HAS_FLAG(T value, T mask) {
    auto v = std::to_underlying(value);
    auto m = std::to_underlying(mask);
    return (v & m) == m;
}

template <typename T>
struct is_bitmask_enum : std::false_type {};

template <typename T>
using enable_if_bitmask_t = std::enable_if_t<is_bitmask_enum<T>::value, T>;

#define ENABLE_BITMASK_OPERATORS(x) \
    template <>                     \
    struct is_bitmask_enum<x> : std::true_type {};

// Bitwise OR
template <typename T>
constexpr enable_if_bitmask_t<T> operator|(T lhs, T rhs) {
    using U = std::underlying_type_t<T>;
    return static_cast<T>(static_cast<U>(lhs) | static_cast<U>(rhs));
}

// Bitwise AND
template <typename T>
constexpr enable_if_bitmask_t<T> operator&(T lhs, T rhs) {
    using U = std::underlying_type_t<T>;
    return static_cast<T>(static_cast<U>(lhs) & static_cast<U>(rhs));
}

// Bitwise XOR
template <typename T>
constexpr enable_if_bitmask_t<T> operator^(T lhs, T rhs) {
    using U = std::underlying_type_t<T>;
    return static_cast<T>(static_cast<U>(lhs) ^ static_cast<U>(rhs));
}

// Bitwise NOT
template <typename T>
constexpr enable_if_bitmask_t<T> operator~(T val) {
    using U = std::underlying_type_t<T>;
    return static_cast<T>(~static_cast<U>(val));
}

// Assignment OR
template <typename T>
constexpr enable_if_bitmask_t<T>& operator|=(T& lhs, T rhs) {
    lhs = lhs | rhs;
    return lhs;
}


template <typename T>
inline T swapEndianness(T val) {
    if constexpr (std::is_floating_point<T>::value) {
        union {
            T f;
            uint32_t i;
        } bits;

        bits.f = val;
        bits.i = (bits.i & 0x000000FF) << 24 | (bits.i & 0x0000FF00) << 8  | (bits.i & 0x00FF0000) >> 8  | (bits.i & 0xFF000000) >> 24;

        return bits.f;
    }
    else if constexpr (std::is_integral<T>::value) {
        if constexpr (sizeof(T) == 1) {
            return val;
        }
        else if constexpr (sizeof(T) == 2) {
            return static_cast<T>((val << 8) | (val >> 8));
        }
        else if constexpr (sizeof(T) == 4) {
            return ((val & 0x000000FF) << 24) | ((val & 0x0000FF00) <<  8) | ((val & 0x00FF0000) >>  8) | ((val & 0xFF000000) >> 24);
        }
        else {
            union U {
                T val;
                std::array<std::uint8_t, sizeof(T)> raw;
            } src, dst;

            src.val = val;
            std::reverse_copy(src.raw.begin(), src.raw.end(), dst.raw.begin());
            return dst.val;
        }
    }
    else {
        union U {
            T val;
            std::array<std::uint8_t, sizeof(T)> raw;
        } src, dst;

        src.val = val;
        std::reverse_copy(src.raw.begin(), src.raw.end(), dst.raw.begin());
        return dst.val;
    }
}

// Tag marker. Previously this was an empty base class that BE-wrapping structs
// inherited from to be picked up by is_BEType_v. On MSVC that subobject was
// elided under #pragma pack(1) (EBO), but Linux clang allocates 1 byte plus
// trailing padding for each empty base, growing every BE struct by 4 bytes
// and breaking the Wii U memory layout. We now detect BE wrappers by an inner
// `using is_be_type = BETypeTag` alias instead of inheritance.
struct BETypeTag {};

template<typename T>
struct BEType {
    using is_be_type = BETypeTag;
    T val;

    BEType() = default;

    BEType(T x) : val(swapEndianness(x)) {}

    explicit operator T() {
        return swapEndianness(val);
    }

    BEType<T>& operator =(T x) {
        val = swapEndianness(x);
        return *this;
    }

    BEType<T>& operator =(const BEType<T>& other) {
        val = other.val;
        return *this;
    }

    T getLE() const {
        return swapEndianness(val);
    }

    T getBE() const {
        return val;
    }


    bool operator ==(const BEType<T>& other) const { return val == other.val; }
    bool operator ==(const T& other) const { return swapEndianness(val) == other; }
    friend bool operator ==(const T& lhs, const BEType<T>& rhs) { return lhs == swapEndianness(rhs.val);}

    bool operator !=(const BEType<T>& other) const { return val != other.val; }
    bool operator !=(const T& other) const { return swapEndianness(val) != other.val; }
    friend bool operator !=(const T& lhs, const BEType<T>& rhs) { return lhs != swapEndianness(rhs.val); }

    bool operator <(const BEType<T>& other) const { return swapEndianness(val) < swapEndianness(other.val); }
    bool operator <(const T& other) const { return swapEndianness(val) < other; }
    friend bool operator <(const T& lhs, const BEType<T>& rhs) { return lhs < swapEndianness(rhs.val); }

    bool operator >(const BEType<T>& other) const { return swapEndianness(val) > swapEndianness(other.val); }
    bool operator >(const T& other) const { return swapEndianness(val) > other; }
    friend bool operator >(const T& lhs, const BEType<T>& rhs) { return lhs > swapEndianness(rhs.val); }

    bool operator <=(const BEType<T>& other) const { return swapEndianness(val) <= swapEndianness(other.val); }
    bool operator <=(const T& other) const { return swapEndianness(val) <= other; }
    friend bool operator <=(const T& lhs, const BEType<T>& rhs) { return lhs <= swapEndianness(rhs.val); }

    bool operator >=(const BEType<T>& other) const { return swapEndianness(val) >= swapEndianness(other.val); }
    bool operator >=(const T& other) const { return swapEndianness(val) >= other; }
    friend bool operator >=(const T& lhs, const BEType<T>& rhs) { return lhs >= swapEndianness(rhs.val); }
};


template<typename T, typename = void>
inline constexpr bool is_BEType_v = false;

template<typename T>
inline constexpr bool is_BEType_v<T, std::void_t<typename T::is_be_type>> = true;

struct BEVec2 {
    using is_be_type = BETypeTag;
    BEType<float> x;
    BEType<float> y;

    BEVec2() = default;
    BEVec2(float x, float y): x(x), y(y) {}
    BEVec2(BEType<float> x, BEType<float> y): x(x), y(y) {}

    glm::fvec2 getLE() const {
        return { x.getLE(), y.getLE() };
    }
};

struct BEVec3 {
    using is_be_type = BETypeTag;
    BEType<float> x;
    BEType<float> y;
    BEType<float> z;

    BEVec3() = default;
    BEVec3(BEType<float> x, BEType<float> y, BEType<float> z): x(x), y(y), z(z) {}
    BEVec3(float x, float y, float z): x(x), y(y), z(z) {}

    float DistanceSq(BEVec3 other) const {
        return (x.getLE() - other.x.getLE()) * (x.getLE() - other.x.getLE()) + (y.getLE() - other.y.getLE()) * (y.getLE() - other.y.getLE()) + (z.getLE() - other.z.getLE()) * (z.getLE() - other.z.getLE());
    }

    glm::fvec3 getLE() const {
        return { x.getLE(), y.getLE(), z.getLE() };
    }

    bool operator==(const BEVec3& other) const {
        return x == other.x && y == other.y && z == other.z;
    }

    void operator=(const glm::fvec3& other) {
        x = other.x;
        y = other.y;
        z = other.z;
    }
};

struct BEMatrix34 {
    using is_be_type = BETypeTag;
    BEType<float> x_x;
    BEType<float> y_x;
    BEType<float> z_x;
    BEType<float> pos_x;
    BEType<float> x_y;
    BEType<float> y_y;
    BEType<float> z_y;
    BEType<float> pos_y;
    BEType<float> x_z;
    BEType<float> y_z;
    BEType<float> z_z;
    BEType<float> pos_z;

    BEMatrix34() = default;

    BEMatrix34(const glm::fvec3& pos, const glm::fquat& quat) {
        setPos(pos);
        setRotLE(quat);
    }
    BEMatrix34(const glm::mat4x3& mat) {
        setLEMatrix(mat);
    }

    float DistanceSq(const BEMatrix34& other) const {
        return (pos_x.getLE() - other.pos_x.getLE()) * (pos_x.getLE() - other.pos_x.getLE()) + (pos_y.getLE() - other.pos_y.getLE()) * (pos_y.getLE() - other.pos_y.getLE()) + (pos_z.getLE() - other.pos_z.getLE()) * (pos_z.getLE() - other.pos_z.getLE());
    }

    std::array<std::array<float, 4>, 3> getLE() const {
        std::array row0 = { x_x.getLE(), y_x.getLE(), z_x.getLE(), pos_x.getLE() };
        std::array row1 = { x_y.getLE(), y_y.getLE(), z_y.getLE(), pos_y.getLE() };
        std::array row2 = { x_z.getLE(), y_z.getLE(), z_z.getLE(), pos_z.getLE() };
        return { row0, row1, row2 };
    }

    glm::mat4x3 getLEMatrix() const {
        return glm::mat4x3(
            glm::vec3(x_x.getLE(), x_y.getLE(), x_z.getLE()),      // X basis column
            glm::vec3(y_x.getLE(), y_y.getLE(), y_z.getLE()),      // Y basis column
            glm::vec3(z_x.getLE(), z_y.getLE(), z_z.getLE()),      // Z basis column
            glm::vec3(pos_x.getLE(), pos_y.getLE(), pos_z.getLE()) // translation column
        );
    }

    void setLEMatrix(const glm::mat4x3& m) {
        // m[col][row]
        x_x = m[0][0];
        x_y = m[0][1];
        x_z = m[0][2];
        y_x = m[1][0];
        y_y = m[1][1];
        y_z = m[1][2];
        z_x = m[2][0];
        z_y = m[2][1];
        z_z = m[2][2];

        pos_x = m[3][0];
        pos_y = m[3][1];
        pos_z = m[3][2];
    }

    BEVec3 getPos() const {
        return { pos_x, pos_y, pos_z };
    }

    void setPos(glm::fvec3 pos) {
        pos_x = pos.x;
        pos_y = pos.y;
        pos_z = pos.z;
    }

    glm::fquat getRotLE() const {
        return glm::quat_cast(glm::fmat3(getLEMatrix()));
    }

	void setRotLE(const glm::fquat& rotation) {
        glm::fmat3 rotMat = glm::mat3_cast(rotation);

        x_x = rotMat[0][0];
        y_x = rotMat[1][0];
        z_x = rotMat[2][0];
        x_y = rotMat[0][1];
        y_y = rotMat[1][1];
        z_y = rotMat[2][1];
        x_z = rotMat[0][2];
        y_z = rotMat[1][2];
        z_z = rotMat[2][2];
    }
};

struct BEMatrix44 {
    using is_be_type = BETypeTag;
    BEType<float> a00;
    BEType<float> a01;
    BEType<float> a02;
    BEType<float> a03;
    BEType<float> a10;
    BEType<float> a11;
    BEType<float> a12;
    BEType<float> a13;
    BEType<float> a20;
    BEType<float> a21;
    BEType<float> a22;
    BEType<float> a23;
    BEType<float> a30;
    BEType<float> a31;
    BEType<float> a32;
    BEType<float> a33;

    BEMatrix44() = default;

    glm::fmat4 getLE() const {
        return glm::fmat4(
            a00.getLE(), a01.getLE(), a02.getLE(), a03.getLE(),
            a10.getLE(), a11.getLE(), a12.getLE(), a13.getLE(),
            a20.getLE(), a21.getLE(), a22.getLE(), a23.getLE(),
            a30.getLE(), a31.getLE(), a32.getLE(), a33.getLE()
        );
    }

    void operator=(glm::fmat4 mtx) {
        a00 = mtx[0][0];
        a01 = mtx[0][1];
        a02 = mtx[0][2];
        a03 = mtx[0][3];
        a10 = mtx[1][0];
        a11 = mtx[1][1];
        a12 = mtx[1][2];
        a13 = mtx[1][3];
        a20 = mtx[2][0];
        a21 = mtx[2][1];
        a22 = mtx[2][2];
        a23 = mtx[2][3];
        a30 = mtx[3][0];
        a31 = mtx[3][1];
        a32 = mtx[3][2];
        a33 = mtx[3][3];
    }
};



#pragma pack(push, 1)
struct BESeadProjection {
    BEType<bool> dirty;
    BEType<bool> deviceDirty;
    BEType<uint8_t> pad0;
    BEType<uint8_t> pad1;
    BEMatrix44 matrix;
    BEMatrix44 deviceMatrix;
    BEType<uint32_t> devicePosture;
    BEType<float> deviceZScale;
    BEType<float> deviceZOffset;
    BEType<uint32_t> __vftable;
};

struct BESeadPerspectiveProjection : BESeadProjection {
    BEType<float> zNear;
    BEType<float> zFar;
    BEType<float> fovYRadiansOrAngle;
    BEType<float> fovySin;
    BEType<float> fovyCos;
    BEType<float> fovyTan;
    BEType<float> aspect;
    BEVec2 offset;
};
#pragma pack(pop)
// BE-wrapped structs no longer inherit from a marker base, so their layouts
// match the Wii U memory layout on both MSVC and Linux clang under
// #pragma pack(1). Size checks are now unconditional.
#define BVR_SIZE_CHECK(expr, msg) static_assert(expr, msg)

BVR_SIZE_CHECK(sizeof(BESeadProjection) == 0x94, "BESeadProjection size mismatch");
BVR_SIZE_CHECK(sizeof(BESeadPerspectiveProjection) == 0xB8, "BESeadPerspectiveProjection size mismatch");

struct data_VRProjectionMatrixOut {
    BEType<float> aspectRatio;
    BEType<float> fovY;
    BEType<float> offsetX;
    BEType<float> offsetY;
};

#include "game_structs.h"
#include "cemu.h"
#include "utils/logger.h"
#include "utils/profiler.h"
