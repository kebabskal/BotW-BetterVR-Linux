#pragma once
#include "vkroots.h"
#include <fstream>

template <>
struct std::formatter<VkResult> : std::formatter<string> {
    auto format(const VkResult format, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{} ({})", std::to_underlying(format), vkroots::helpers::enumString(format));
    }
};

template <>
struct std::formatter<XrResult> : std::formatter<string> {
    auto format(const XrResult format, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", std::to_underlying(format));
    }
};

template <>
struct std::formatter<VkFormat> : std::formatter<string> {
    auto format(const VkFormat format, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{} ({})", std::to_underlying(format), vkroots::helpers::enumString(format));
    }
};

template <>
struct std::formatter<DXGI_FORMAT> : std::formatter<string> {
    auto format(const DXGI_FORMAT format, std::format_context& ctx) const {
        std::format_to(ctx.out(), "{}", std::to_underlying(format));
        switch (format) {
            case DXGI_FORMAT_UNKNOWN: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_UNKNOWN)");
            }
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)");
            }
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_B8G8R8A8_UNORM_SRGB)");
            }
            case DXGI_FORMAT_D32_FLOAT: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_D32_FLOAT)");
            }
            case DXGI_FORMAT_D16_UNORM: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_D16_UNORM)");
            }
            case DXGI_FORMAT_R32G32B32_TYPELESS: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_R32G32B32_TYPELESS)");
            }
            case DXGI_FORMAT_D24_UNORM_S8_UINT: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_D24_UNORM_S8_UINT)");
            }
            case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: {
                return std::format_to(ctx.out(), " (DXGI_FORMAT_D32_FLOAT_S8X24_UINT)");
            }
        }
    }
};

template <>
struct std::formatter<glm::fmat3> : std::formatter<string> {
    auto format(const glm::fmat3& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(mtx));
    }
};

template <>
struct std::formatter<glm::fmat4> : std::formatter<string> {
    auto format(const glm::fmat4& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(mtx));
    }
};

template <>
struct std::formatter<glm::fmat3x4> : std::formatter<string> {
    auto format(const glm::fmat3x4& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(mtx));
    }
};

template <>
struct std::formatter<glm::mat4x3> : std::formatter<string> {
    auto format(const glm::mat4x3& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(mtx));
    }
};



template <>
struct std::formatter<glm::fvec2> : std::formatter<string> {
    auto format(const glm::fvec2& vec, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(vec));
    }
};

template <>
struct std::formatter<glm::fvec3> : std::formatter<string> {
    auto format(const glm::fvec3& vec, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(vec));
    }
};

template <>
struct std::formatter<glm::fvec4> : std::formatter<string> {
    auto format(const glm::fvec4& vec, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(vec));
    }
};

template <>
struct std::formatter<glm::fquat> : std::formatter<string> {
    auto format(const glm::fquat& quat, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "[w={:.1f}, x={:.1f}, y={:.1f}, z={:.1f}] (euler: x={:.1f}, y={:.1f}, z={:.1f})", quat.w, quat.x, quat.y, quat.z, glm::degrees(glm::eulerAngles(quat)).x, glm::degrees(glm::eulerAngles(quat)).y, glm::degrees(glm::eulerAngles(quat)).z);
    }
};

template <>
struct std::formatter<BEVec3> : std::formatter<string> {
    auto format(const BEVec3& vec, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(vec.getLE()));
    }
};

template <>
struct std::formatter<BEMatrix34> : std::formatter<string> {
    auto format(const BEMatrix34& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "[x_x={:.1f}, y_x={:.1f}, z_x={:.1f}, pos_x={:.1f}] [x_y={:.1f}, x_y={:.1f}, z_y={:.1f}, pos_y={:.1f}] [x_z={:.1f}, y_z={:.1f}, z_z={:.1f}, pos_z={:.1f}]",
            mtx.x_x.getLE(), mtx.y_x.getLE(), mtx.z_x.getLE(), mtx.pos_x.getLE(),
            mtx.x_y.getLE(), mtx.y_y.getLE(), mtx.z_y.getLE(), mtx.pos_y.getLE(),
            mtx.x_z.getLE(), mtx.y_z.getLE(), mtx.z_z.getLE(), mtx.pos_z.getLE()
        );
    }
};

template <>
struct std::formatter<BEMatrix44> : std::formatter<string> {
    auto format(const BEMatrix44& mtx, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "{}", glm::to_string(mtx.getLE()));
    }
};

template <>
struct std::formatter<BESeadProjection> : std::formatter<string> {
    auto format(const BESeadProjection& proj, std::format_context& ctx) const {
        return std::format_to(ctx.out(),
            "vtable = {:08X}, dirty = {}, deviceDirty = {}, matrix = {}, deviceMatrix = {} devicePosture = {}, deviceZOffset = {}, deviceZScale = {}",
            proj.__vftable.getLE(), proj.dirty.getLE(), proj.deviceDirty.getLE(), proj.matrix, proj.deviceMatrix, proj.devicePosture.getLE(), proj.deviceZOffset.getLE(), proj.deviceZScale.getLE()
        );
    }
};

template <>
struct std::formatter<BESeadPerspectiveProjection> : std::formatter<string> {
    auto format(const BESeadPerspectiveProjection& proj, std::format_context& ctx) const {
        return std::format_to(ctx.out(),
            "near = {}, far = {}, angle = {}, fovySin = {}, fovyCos = {}, fovyTan = {}, aspect = {}, offsetX = {}, offsetY = {}\r\ndirty = {}, deviceDirty = {}, matrix = {}, deviceMatrix = {} devicePosture = {}, deviceZOffset = {}, deviceZScale = {}",
            proj.zNear.getLE(), proj.zFar.getLE(), proj.fovYRadiansOrAngle.getLE(), proj.fovySin.getLE(), proj.fovyCos.getLE(), proj.fovyTan.getLE(), proj.aspect.getLE(), proj.offset.x.getLE(), proj.offset.y.getLE(),
            proj.dirty.getLE(), proj.deviceDirty.getLE(), proj.matrix, proj.deviceMatrix, proj.devicePosture.getLE(), proj.deviceZOffset.getLE(), proj.deviceZScale.getLE()
        );
    }
};

template <>
struct std::formatter<BESeadCamera> : std::formatter<string> {
    auto format(const BESeadCamera& cam, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "mtx = {}, vtbl = {:08X}", cam.mtx, cam.__vftable.getLE());
    }
};

template <>
struct std::formatter<BESeadLookAtCamera> : std::formatter<string> {
    auto format(const BESeadLookAtCamera& cam, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "mtx = {}, pos = {}, at = {}, up = {}", cam.mtx, cam.pos, cam.at, cam.up);
    }
};

template <>
struct std::formatter<LookAtMatrix> : std::formatter<string> {
    auto format(const LookAtMatrix& cam, std::format_context& ctx) const {
        return std::format_to(ctx.out(), "pos = {}, target = {}, up = {}, unknown = {}, zNear = {}, zFar = {}", cam.pos, cam.target, cam.up, cam.unknown, cam.zNear.getLE(), cam.zFar.getLE());
    }
};


template <>
struct std::formatter<D3D_FEATURE_LEVEL> : std::formatter<string> {
    auto format(const D3D_FEATURE_LEVEL featureLevel, std::format_context& ctx) const {
        switch (featureLevel) {
            case D3D_FEATURE_LEVEL_1_0_CORE:
                return std::format_to(ctx.out(), "1.0");
            case D3D_FEATURE_LEVEL_9_1:
                return std::format_to(ctx.out(), "9.1");
            case D3D_FEATURE_LEVEL_9_2:
                return std::format_to(ctx.out(), "9.2");
            case D3D_FEATURE_LEVEL_9_3:
                return std::format_to(ctx.out(), "9.3");
            case D3D_FEATURE_LEVEL_10_0:
                return std::format_to(ctx.out(), "10.0");
            case D3D_FEATURE_LEVEL_10_1:
                return std::format_to(ctx.out(), "10.1");
            case D3D_FEATURE_LEVEL_11_0:
                return std::format_to(ctx.out(), "11.0");
            case D3D_FEATURE_LEVEL_11_1:
                return std::format_to(ctx.out(), "11.1");
            case D3D_FEATURE_LEVEL_12_0:
                return std::format_to(ctx.out(), "12.0");
            case D3D_FEATURE_LEVEL_12_1:
                return std::format_to(ctx.out(), "12.1");
            default:
                break;
        }
        return std::format_to(ctx.out(), "{:X}", std::to_underlying(featureLevel));
    }
};

static std::string FormatDistance(float distance) {
    float distanceInches = distance * 39.3700787f;
    int32_t distanceFeet = std::floor(distanceInches / 12.0f);
    distanceInches -= distanceFeet * 12.0f;
    return std::format("{:.02f}m / {}\' {:.02f}\"", distance, distanceFeet, distanceInches);
}

enum class LogType {
    // verbose logging types
    RENDERING,
    INTEROP,
    CONTROLS,
    PPC,
    XR_DEBUGUTILS,

    // generic types
    INFO,
    WARNING,
    ERROR,
    VERBOSE
};

using enum LogType;
ENABLE_BITMASK_OPERATORS(LogType);

class Log {
public:
    Log();
    ~Log();

    template <LogType L>
    static inline bool consteval isLogTypeEnabled() {
        if constexpr (L == ERROR) {
            return true;
        }
        if constexpr (L == WARNING) {
            return true;
        }
        if constexpr (L == INFO) {
            return true;
        }
#ifdef _DEBUG
        if constexpr (L == VERBOSE) {
            return true;
        }
        //if constexpr (L == RENDERING) {
        //    return true;
        //}
        //if constexpr (L == CONTROLS) {
        //    return true;
        //}
        //if constexpr (L == PPC) {
        //    return true;
        //}
#endif
        return false;
    }

    template <LogType L>
    static inline void print(const char* message) {
        if constexpr (!isLogTypeEnabled<L>()) {
            return;
        }
        std::lock_guard<std::mutex> lock(logMutex);
        std::string messageStr = std::string(message) + "\n";

#ifndef _DEBUG
        if (logFile.is_open()) {
            logFile << messageStr;
            logFile.flush();
        }
#endif

        DWORD charsWritten = 0;
        WriteConsoleA(consoleHandle, messageStr.c_str(), (DWORD)messageStr.size(), &charsWritten, NULL);
#ifdef _DEBUG
        //std::string messageStr = std::string(message) + "\n";
        //DWORD charsWritten = 0;
        //WriteConsoleA(consoleHandle, messageStr.c_str(), (DWORD)messageStr.size(), &charsWritten, NULL);
        OutputDebugStringA(messageStr.c_str());
#else
        std::cout << message << std::endl;
#endif
    }

    template <LogType L, class... Args>
    static inline void print(const char* format, Args&&... args) {
        if constexpr (!isLogTypeEnabled<L>()) {
            return;
        }
        Log::print<L>(std::vformat(format, std::make_format_args(args...)).c_str());
    }

    static void printTimeElapsed(const char* message_prefix, LARGE_INTEGER time);

private:
    static HANDLE consoleHandle;
    static double timeFrequency;
    static std::ofstream logFile;
    static std::mutex logMutex;
};

static void checkXRResult(const XrResult result, const char* errorMessage) {
    if (XR_FAILED(result)) {
        if (errorMessage == nullptr) {
            Log::print<ERROR>("An unknown error (result was {}) has occurred!", result);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, std::format("An unknown error {} has occurred which caused a fatal crash!", result).c_str(), "An error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error("Unidentified error occurred!");
        }
        else {
            Log::print<ERROR>("Error {}: {}", result, errorMessage);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, errorMessage, "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error(errorMessage);
        }
    }
}

static void checkHResult(const HRESULT result, const char* errorMessage) {
    if (FAILED(result)) {
        if (errorMessage == nullptr) {
            Log::print<ERROR>("[Error] An unknown error (result was {}) has occurred!", result);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, std::format("An unknown error {} has occurred which caused a fatal crash!", result).c_str(), "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error("Unidentified error occurred!");
        }
        else {
            Log::print<ERROR>("Error {}: {}", result, errorMessage);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, errorMessage, "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error(errorMessage);
        }
    }
}

static void checkVkResult(const VkResult result, const char* errorMessage) {
    if (result != VK_SUCCESS) {
        if (errorMessage == nullptr) {
            Log::print<ERROR>("An unknown error (result was {}) has occurred!", (std::underlying_type_t<VkResult>)result);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, std::format("An unknown error {} has occurred which caused a fatal crash!", (std::underlying_type_t<VkResult>)result).c_str(), "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error("Unidentified error occurred!");
        }
        else {
            Log::print<ERROR>("Error {}: {}", (std::underlying_type_t<VkResult>)result, errorMessage);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, errorMessage, "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error(errorMessage);
        }
    }
}

static void checkAssert(const bool assert, const char* errorMessage) {
    if (!assert) {
        if (errorMessage == nullptr) {
            Log::print<ERROR>("Something unexpected happened that prevents further execution!");
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, "Something unexpected happened that prevents further execution!", "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error("Unexpected assertion occurred!");
        }
        else {
            Log::print<ERROR>("{}", errorMessage);
#ifdef _DEBUG
            __debugbreak();
#endif
            MessageBoxA(NULL, errorMessage, "A fatal error occurred!", MB_OK | MB_ICONERROR);
            throw std::runtime_error(errorMessage);
        }
    }
}