#pragma once

#include "pch.h"
#include "utils/vulkan_utils.h"
#include <shaderc/shaderc.hpp>

// Tiny helper for runtime GLSL → SPIR-V compilation. Mirrors upstream's
// D3D12Utils::CompileShader signature so the call sites read the same.
namespace ShaderUtils {

inline std::vector<uint32_t> CompileGLSL(const char* source, shaderc_shader_kind kind, const char* name) {
    static thread_local shaderc::Compiler compiler;
    static thread_local shaderc::CompileOptions options = []() {
        shaderc::CompileOptions o;
        o.SetSourceLanguage(shaderc_source_language_glsl);
        o.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        o.SetTargetSpirv(shaderc_spirv_version_1_5);
        o.SetOptimizationLevel(shaderc_optimization_level_performance);
        return o;
    }();

    auto result = compiler.CompileGlslToSpv(source, std::strlen(source), kind, name, "main", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        Log::print<ERROR>("Shader compile failed for {}: {}", name, result.GetErrorMessage());
        checkAssert(false, std::format("Shader compile failed: {}", name).c_str());
    }
    return { result.cbegin(), result.cend() };
}

inline VkShaderModule MakeShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    checkVkResult(vkCreateShaderModule(device, &ci, nullptr, &mod), "Failed to create VkShaderModule");
    return mod;
}

} // namespace ShaderUtils
