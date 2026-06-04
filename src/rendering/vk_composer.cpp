// Linux Vulkan-side compositor backend. 1:1 port of d3d12.cpp.
//
// Both files own the GPU-side composition for VR submission. D3D12 used a
// dedicated D3D12 device (matched by LUID to OpenXR's chosen GPU) and managed
// command queues/lists/fences. Vulkan equivalent: a dedicated VkInstance +
// VkDevice (built by xrCreateVulkan{Instance,Device}KHR so they're guaranteed
// to satisfy OpenXR's graphics requirements), VkQueue, VkCommandPool +
// reusable VkCommandBuffers, and a timeline VkSemaphore for queue completion.

#include "pch.h"

#include "vk_composer.h"
#include "instance.h"
#include "shader.h"
#include "utils/shader_utils.h"
#include "utils/vulkan_utils.h"

// Vulkan loader symbols. On the composer's VkInstance/VkDevice we can use the
// loader's prototypes directly (we link against libvulkan). vkroots' dispatch
// tables are used only for Cemu's instance/device (where re-entrancy matters);
// the composer is a separate stack.

namespace {
    // OpenXR Vulkan2 extension entrypoints, looked up lazily on the XrInstance.
    PFN_xrGetVulkanGraphicsRequirements2KHR pfn_xrGetVulkanGraphicsRequirements2KHR = nullptr;
    PFN_xrCreateVulkanInstanceKHR           pfn_xrCreateVulkanInstanceKHR = nullptr;
    PFN_xrCreateVulkanDeviceKHR             pfn_xrCreateVulkanDeviceKHR = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR       pfn_xrGetVulkanGraphicsDevice2KHR = nullptr;

    void LoadXrVulkanExtFns(XrInstance xr) {
        if (pfn_xrCreateVulkanInstanceKHR) return;
        checkXRResult(xrGetInstanceProcAddr(xr, "xrGetVulkanGraphicsRequirements2KHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfn_xrGetVulkanGraphicsRequirements2KHR)),
            "xrGetInstanceProcAddr xrGetVulkanGraphicsRequirements2KHR");
        checkXRResult(xrGetInstanceProcAddr(xr, "xrCreateVulkanInstanceKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfn_xrCreateVulkanInstanceKHR)),
            "xrGetInstanceProcAddr xrCreateVulkanInstanceKHR");
        checkXRResult(xrGetInstanceProcAddr(xr, "xrCreateVulkanDeviceKHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfn_xrCreateVulkanDeviceKHR)),
            "xrGetInstanceProcAddr xrCreateVulkanDeviceKHR");
        checkXRResult(xrGetInstanceProcAddr(xr, "xrGetVulkanGraphicsDevice2KHR",
            reinterpret_cast<PFN_xrVoidFunction*>(&pfn_xrGetVulkanGraphicsDevice2KHR)),
            "xrGetInstanceProcAddr xrGetVulkanGraphicsDevice2KHR");
    }
}

// ===================================================================== //
// RND_VkComposer ctor/dtor
// ===================================================================== //

RND_VkComposer::RND_VkComposer() {
    CreateInstanceAndDevice();

    // Memory props (used by Texture allocations).
    vkGetPhysicalDeviceMemoryProperties(m_physicalDevice, &m_memoryProperties);

    // Queue completion timeline semaphore (replaces D3D12 fence + event).
    m_queueTimeline = CreateTimelineSemaphore(0);
    m_nextFenceValue = 1;

    // Per-frame command pools + reusable command buffers.
    auto allocCmdBuffer = [&](VkCommandPool pool, VkCommandBuffer& out) {
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        checkVkResult(vkAllocateCommandBuffers(m_device, &ai, &out), "alloc reusable cmd buffer");
    };

    VkCommandPoolCreateInfo poolCi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    poolCi.queueFamilyIndex = m_queueFamilyIndex;
    poolCi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                   VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;

    for (auto& frameCtx : m_frameContexts) {
        checkVkResult(vkCreateCommandPool(m_device, &poolCi, nullptr, &frameCtx.commandPool),
                      "create frame command pool");
        for (auto& cb : frameCtx.commandBuffers) {
            allocCmdBuffer(frameCtx.commandPool, cb);
        }
    }
    checkVkResult(vkCreateCommandPool(m_device, &poolCi, nullptr, &m_immediateContext.commandPool),
                  "create immediate command pool");
    allocCmdBuffer(m_immediateContext.commandPool, m_immediateContext.commandBuffer);

    Log::print<INFO>("RND_VkComposer initialized — VkDevice={}, queueFamily={}",
                     (void*)m_device, m_queueFamilyIndex);
}

RND_VkComposer::~RND_VkComposer() {
    if (m_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(m_device);
        for (auto& frameCtx : m_frameContexts) {
            if (frameCtx.commandPool) vkDestroyCommandPool(m_device, frameCtx.commandPool, nullptr);
        }
        if (m_immediateContext.commandPool) vkDestroyCommandPool(m_device, m_immediateContext.commandPool, nullptr);
        if (m_queueTimeline) vkDestroySemaphore(m_device, m_queueTimeline, nullptr);
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_instance != VK_NULL_HANDLE) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

// ===================================================================== //
// Instance + device creation via OpenXR's Vulkan2 helpers.
// ===================================================================== //

void RND_VkComposer::CreateInstanceAndDevice() {
    // OpenXR (m_instance + m_systemId) was created in OpenXR ctor (called
    // from VRManager ctor before this constructor). It owns the runtime
    // version negotiation.
    auto& xr = VRManager::instance().XR;
    checkAssert(xr && xr->m_instance != XR_NULL_HANDLE, "OpenXR XrInstance is not initialized before RND_VkComposer");
    checkAssert(xr->m_systemId != XR_NULL_SYSTEM_ID, "OpenXR XrSystemId is not initialized before RND_VkComposer");
    LoadXrVulkanExtFns(xr->m_instance);

    XrGraphicsRequirementsVulkan2KHR vkReqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR };
    checkXRResult(pfn_xrGetVulkanGraphicsRequirements2KHR(xr->m_instance, xr->m_systemId, &vkReqs),
                  "xrGetVulkanGraphicsRequirements2KHR");
    Log::print<INFO>("XR Vulkan requirements: min API {}.{}.{}, max API {}.{}.{}",
        XR_VERSION_MAJOR(vkReqs.minApiVersionSupported),
        XR_VERSION_MINOR(vkReqs.minApiVersionSupported),
        XR_VERSION_PATCH(vkReqs.minApiVersionSupported),
        XR_VERSION_MAJOR(vkReqs.maxApiVersionSupported),
        XR_VERSION_MINOR(vkReqs.maxApiVersionSupported),
        XR_VERSION_PATCH(vkReqs.maxApiVersionSupported));

    // --- VkInstance ---
    VkApplicationInfo appInfo = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    appInfo.pApplicationName = "BotW-BetterVR Composer";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 9, 16);
    appInfo.pEngineName = "BotW-BetterVR";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 9, 16);
    appInfo.apiVersion = VK_API_VERSION_1_2;

    const char* instanceExts[] = {
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    };
    VkInstanceCreateInfo instCi = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instCi.pApplicationInfo = &appInfo;
    instCi.enabledExtensionCount = static_cast<uint32_t>(std::size(instanceExts));
    instCi.ppEnabledExtensionNames = instanceExts;

    XrVulkanInstanceCreateInfoKHR xrInstCi = { XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR };
    xrInstCi.systemId = xr->m_systemId;
    xrInstCi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xrInstCi.vulkanCreateInfo = &instCi;
    xrInstCi.vulkanAllocator = nullptr;

    VkResult vkRes = VK_SUCCESS;
    checkXRResult(pfn_xrCreateVulkanInstanceKHR(xr->m_instance, &xrInstCi, &m_instance, &vkRes),
                  "xrCreateVulkanInstanceKHR");
    checkVkResult(vkRes, "vkCreateInstance (via xrCreateVulkanInstanceKHR)");

    // --- VkPhysicalDevice ---
    XrVulkanGraphicsDeviceGetInfoKHR xrDevGetInfo = { XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR };
    xrDevGetInfo.systemId = xr->m_systemId;
    xrDevGetInfo.vulkanInstance = m_instance;
    checkXRResult(pfn_xrGetVulkanGraphicsDevice2KHR(xr->m_instance, &xrDevGetInfo, &m_physicalDevice),
                  "xrGetVulkanGraphicsDevice2KHR");

    VkPhysicalDeviceProperties props = {};
    vkGetPhysicalDeviceProperties(m_physicalDevice, &props);
    Log::print<INFO>("RND_VkComposer using VkPhysicalDevice {} (driver {}.{}.{})",
                     props.deviceName,
                     VK_VERSION_MAJOR(props.driverVersion),
                     VK_VERSION_MINOR(props.driverVersion),
                     VK_VERSION_PATCH(props.driverVersion));

    // --- Queue family (graphics + transfer) ---
    uint32_t qfCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount, nullptr);
    std::vector<VkQueueFamilyProperties> qfProps(qfCount);
    vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &qfCount, qfProps.data());
    m_queueFamilyIndex = UINT32_MAX;
    for (uint32_t i = 0; i < qfCount; ++i) {
        if (qfProps[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { m_queueFamilyIndex = i; break; }
    }
    checkAssert(m_queueFamilyIndex != UINT32_MAX, "No graphics queue family on composer device");

    // --- VkDevice ---
    float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo qCi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qCi.queueFamilyIndex = m_queueFamilyIndex;
    qCi.queueCount = 1;
    qCi.pQueuePriorities = &queuePriority;

    const char* deviceExts[] = {
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
        VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
    };

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeat = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES };
    timelineFeat.timelineSemaphore = VK_TRUE;

    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynRenderFeat = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR };
    dynRenderFeat.dynamicRendering = VK_TRUE;
    dynRenderFeat.pNext = &timelineFeat;

    VkDeviceCreateInfo devCi = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    devCi.queueCreateInfoCount = 1;
    devCi.pQueueCreateInfos = &qCi;
    devCi.enabledExtensionCount = static_cast<uint32_t>(std::size(deviceExts));
    devCi.ppEnabledExtensionNames = deviceExts;
    devCi.pNext = &dynRenderFeat;

    XrVulkanDeviceCreateInfoKHR xrDevCi = { XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR };
    xrDevCi.systemId = xr->m_systemId;
    xrDevCi.pfnGetInstanceProcAddr = &vkGetInstanceProcAddr;
    xrDevCi.vulkanPhysicalDevice = m_physicalDevice;
    xrDevCi.vulkanCreateInfo = &devCi;
    xrDevCi.vulkanAllocator = nullptr;

    checkXRResult(pfn_xrCreateVulkanDeviceKHR(xr->m_instance, &xrDevCi, &m_device, &vkRes),
                  "xrCreateVulkanDeviceKHR");
    checkVkResult(vkRes, "vkCreateDevice (via xrCreateVulkanDeviceKHR)");

    vkGetDeviceQueue(m_device, m_queueFamilyIndex, 0, &m_queue);
}

// ===================================================================== //
// Frame lifecycle (mirrors D3D12::StartFrame/EndFrame).
// ===================================================================== //

void RND_VkComposer::StartFrame() {
    m_currentFrameContextIndex = (m_currentFrameContextIndex + 1) % kFrameContextCount;

    FrameContext& frameCtx = GetCurrentFrameContext();
    if (frameCtx.completionFenceValue != 0) {
        WaitForQueueFence(frameCtx.completionFenceValue);
        frameCtx.completionFenceValue = 0;
    }

    // D3D12 reset the allocator (which implicitly invalidated all command lists
    // sourced from it). Vulkan equivalent: reset the command pool.
    checkVkResult(vkResetCommandPool(m_device, frameCtx.commandPool, 0), "vkResetCommandPool");
    frameCtx.nextCommandBufferIndex = 0;
}

void RND_VkComposer::EndFrame() {
    GetCurrentFrameContext().completionFenceValue = SignalQueueFence();
}

RND_VkComposer::FrameContext& RND_VkComposer::GetCurrentFrameContext() {
    return m_frameContexts[m_currentFrameContextIndex];
}

VkCommandBuffer RND_VkComposer::AcquireFrameCommandBuffer() {
    FrameContext& frameCtx = GetCurrentFrameContext();
    checkAssert(frameCtx.nextCommandBufferIndex < frameCtx.commandBuffers.size(),
                "Exceeded the reusable Vulkan frame command buffer pool!");
    VkCommandBuffer cb = frameCtx.commandBuffers[frameCtx.nextCommandBufferIndex++];

    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVkResult(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer frame");
    return cb;
}

VkCommandBuffer RND_VkComposer::AcquireImmediateCommandBuffer() {
    checkVkResult(vkResetCommandPool(m_device, m_immediateContext.commandPool, 0),
                  "vkResetCommandPool immediate");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    checkVkResult(vkBeginCommandBuffer(m_immediateContext.commandBuffer, &bi),
                  "vkBeginCommandBuffer immediate");
    return m_immediateContext.commandBuffer;
}

void RND_VkComposer::ExecuteCommandBuffer(VkCommandBuffer cmdBuffer,
                                          std::span<const std::pair<Texture*, uint64_t>> waitFor,
                                          std::span<const std::pair<Texture*, uint64_t>> signalTo) {
    checkVkResult(vkEndCommandBuffer(cmdBuffer), "vkEndCommandBuffer");

    // Build wait/signal semaphore arrays. Each entry is a Texture's timeline
    // semaphore + the per-call value. Plus we signal our queue timeline so
    // StartFrame can wait on it.
    std::vector<VkSemaphore> waitSems;
    std::vector<uint64_t>    waitVals;
    std::vector<VkPipelineStageFlags> waitStages;
    waitSems.reserve(waitFor.size());
    waitVals.reserve(waitFor.size());
    waitStages.reserve(waitFor.size());
    for (auto& [tex, val] : waitFor) {
        waitSems.push_back(tex->GetTimelineSemaphore());
        waitVals.push_back(val);
        waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    }

    std::vector<VkSemaphore> signalSems;
    std::vector<uint64_t>    signalVals;
    signalSems.reserve(signalTo.size() + 1);
    signalVals.reserve(signalTo.size() + 1);
    for (auto& [tex, val] : signalTo) {
        signalSems.push_back(tex->GetTimelineSemaphore());
        signalVals.push_back(val);
    }
    // Always also signal our queue timeline (for StartFrame to wait on).
    const uint64_t queueValue = m_nextFenceValue++;
    signalSems.push_back(m_queueTimeline);
    signalVals.push_back(queueValue);

    VkTimelineSemaphoreSubmitInfo timelineSi = {
        VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    timelineSi.waitSemaphoreValueCount = (uint32_t)waitVals.size();
    timelineSi.pWaitSemaphoreValues = waitVals.empty() ? nullptr : waitVals.data();
    timelineSi.signalSemaphoreValueCount = (uint32_t)signalVals.size();
    timelineSi.pSignalSemaphoreValues = signalVals.data();

    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &timelineSi;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmdBuffer;
    si.waitSemaphoreCount = (uint32_t)waitSems.size();
    si.pWaitSemaphores = waitSems.empty() ? nullptr : waitSems.data();
    si.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    si.signalSemaphoreCount = (uint32_t)signalSems.size();
    si.pSignalSemaphores = signalSems.data();

    checkVkResult(vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
}

uint64_t RND_VkComposer::SignalQueueFence() {
    // D3D12 had m_queue->Signal(fence, value). For us, command submission
    // already signals m_queueTimeline; this method is kept for parity with
    // upstream's StartFrame/EndFrame, returning the *next* expected value so
    // callers can wait on it later.
    const uint64_t value = m_nextFenceValue;
    // Submit an empty batch to signal m_queueTimeline at value.
    VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
    tsi.signalSemaphoreValueCount = 1;
    tsi.pSignalSemaphoreValues = &value;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.pNext = &tsi;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &m_queueTimeline;
    checkVkResult(vkQueueSubmit(m_queue, 1, &si, VK_NULL_HANDLE), "SignalQueueFence vkQueueSubmit");
    m_nextFenceValue++;
    return value;
}

void RND_VkComposer::WaitForQueueFence(uint64_t fenceValue) {
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
    wi.semaphoreCount = 1;
    wi.pSemaphores = &m_queueTimeline;
    wi.pValues = &fenceValue;
    checkVkResult(vkWaitSemaphores(m_device, &wi, UINT64_MAX), "WaitForQueueFence vkWaitSemaphores");
}

uint32_t RND_VkComposer::FindMemoryType(uint32_t memoryTypeBitsRequirement,
                                        VkMemoryPropertyFlags requirementsMask) const {
    for (uint32_t i = 0; i < m_memoryProperties.memoryTypeCount; ++i) {
        if (!(memoryTypeBitsRequirement & (1u << i))) continue;
        if ((m_memoryProperties.memoryTypes[i].propertyFlags & requirementsMask) == requirementsMask) {
            return i;
        }
    }
    checkAssert(false, "RND_VkComposer::FindMemoryType: no compatible memory type");
    return 0;
}

VkSemaphore RND_VkComposer::CreateTimelineSemaphore(uint64_t initialValue) const {
    VkSemaphoreTypeCreateInfo typeCi = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    typeCi.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    typeCi.initialValue = initialValue;
    VkSemaphoreCreateInfo ci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    ci.pNext = &typeCi;
    VkSemaphore sem = VK_NULL_HANDLE;
    checkVkResult(vkCreateSemaphore(m_device, &ci, nullptr, &sem), "CreateTimelineSemaphore");
    return sem;
}

// ===================================================================== //
// CommandContext<T>::~CommandContext  (template definition out-of-line)
// ===================================================================== //

template <bool blockTillExecuted>
RND_VkComposer::CommandContext<blockTillExecuted>::~CommandContext() {
    // 1) Apply pre-execution waits (texture timelines).
    //    These are already encoded into the submit via Signal/Wait pairs in
    //    ExecuteCommandBuffer, so we don't issue separate waits — but we DO
    //    update the texture's "last awaited/signaled" tracker, which
    //    upstream did inline (texture->d3d12WaitForFence/SignalFence).
    for (auto& [texture, value] : m_waitFor) {
        texture->TrackWaitedValue(value);
    }

    // 2) End and submit the command buffer with the wait/signal sets.
    m_composer->ExecuteCommandBuffer(m_cmdBuffer, m_waitFor, m_signalTo);

    for (auto& [texture, value] : m_signalTo) {
        texture->TrackSignaledValue(value);
    }

    // 3) For immediate / blocking submissions, wait synchronously.
    if constexpr (blockTillExecuted) {
        // The queue timeline was incremented by ExecuteCommandBuffer; wait
        // on the value that submission just enqueued.
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        const uint64_t queueValueJustSignaled = m_composer->m_nextFenceValue - 1;
        wi.semaphoreCount = 1;
        wi.pSemaphores = &m_composer->m_queueTimeline;
        wi.pValues = &queueValueJustSignaled;
        checkVkResult(vkWaitSemaphores(m_composer->m_device, &wi, UINT64_MAX),
                      "CommandContext block wait");
    }
}

// Explicit template instantiations.
template class RND_VkComposer::CommandContext<true>;
template class RND_VkComposer::CommandContext<false>;

// ===================================================================== //
// PresentPipeline<depth>
// ===================================================================== //

namespace {
    // GLSL versions of the present shaders. Hand-translated 1:1 from
    // presentDepthHLSL / presentHLSL in shader.h. Bindings:
    //   set 0 / binding 0: g_colorTexture       (sampler2D)
    //   set 0 / binding 1: g_depthTexture       (sampler2D, depth-only variant)
    //   set 0 / binding 2: g_fadeSampleTexture  (sampler2D, depth-only variant)
    //   set 0 / binding 3: settings UBO         (always)
    constexpr const char* kPresentVertGlsl = R"(
        #version 450
        layout(location = 0) out vec2 v_uv;
        void main() {
            v_uv = vec2(float(gl_VertexIndex % 2),
                        float((gl_VertexIndex % 4) / 2));
            // Vulkan framebuffer is y-down. v_uv (0,0) = texture top-left should
            // map to fb top-left (ndc.y=-1). Upstream HLSL flipped Y for D3D;
            // we don't here, which also reverses triangle winding into CW (matching
            // pipeline's frontFace=CW so cullMode=BACK doesn't discard).
            gl_Position = vec4((v_uv.x - 0.5) * 2.0,
                               (v_uv.y - 0.5) * 2.0, 0.0, 1.0);
        }
    )";

    constexpr const char* kPresentFragGlsl = R"(
        #version 450
        layout(location = 0) in vec2 v_uv;
        layout(location = 0) out vec4 o_color;
        layout(set = 0, binding = 0) uniform sampler2D g_color;
        layout(set = 0, binding = 3) uniform Settings {
            float renderWidth, renderHeight;
            float swapchainWidth, swapchainHeight;
            float uvOffsetX, uvOffsetY;
            float uvScaleX, uvScaleY;
            float customFadeAmount, customFadeColorR;
            float customFadeColorG, customFadeColorB;
            float isFadeActive;
        } S;
        void main() {
            vec2 sp = vec2(S.uvOffsetX, S.uvOffsetY) + v_uv * vec2(S.uvScaleX, S.uvScaleY);
            vec4 c = texture(g_color, sp);
            o_color = c;
        }
    )";

    constexpr const char* kPresentDepthFragGlsl = R"(
        #version 450
        layout(location = 0) in vec2 v_uv;
        layout(location = 0) out vec4 o_color;
        layout(set = 0, binding = 0) uniform sampler2D g_color;
        layout(set = 0, binding = 1) uniform sampler2D g_depth;
        layout(set = 0, binding = 2) uniform sampler2D g_fadeSample;
        layout(set = 0, binding = 3) uniform Settings {
            float renderWidth, renderHeight;
            float swapchainWidth, swapchainHeight;
            float uvOffsetX, uvOffsetY;
            float uvScaleX, uvScaleY;
            float customFadeAmount, customFadeColorR;
            float customFadeColorG, customFadeColorB;
            float isFadeActive;
        } S;
        void main() {
            vec2 sp = vec2(S.uvOffsetX, S.uvOffsetY) + v_uv * vec2(S.uvScaleX, S.uvScaleY);
            vec4 c = texture(g_color, sp);
            float d = texture(g_depth, sp).r;
            vec4 fadeSample = texelFetch(g_fadeSample, ivec2(0), 0);
            float gameFade = S.isFadeActive > 0.5 ? fadeSample.a : 0.0;
            vec3 gameFadeColor = fadeSample.rgb;
            float customFade = clamp(S.customFadeAmount, 0.0, 1.0);
            vec3 customCol = vec3(S.customFadeColorR, S.customFadeColorG, S.customFadeColorB);
            float finalFade = max(gameFade, customFade);
            vec3 finalCol = gameFade >= customFade ? gameFadeColor : customCol;
            o_color = vec4(mix(c.rgb, finalCol, finalFade), c.a);
            gl_FragDepth = d;
        }
    )";

}

template <bool depth>
RND_VkComposer::PresentPipeline<depth>::PresentPipeline(RND_Renderer* pRenderer)
    : m_renderer(pRenderer)
{
    VkDevice dev = VRManager::instance().Composer->GetDevice();

    // --- Compile shaders ---
    m_vertexSpirv = ShaderUtils::CompileGLSL(kPresentVertGlsl, shaderc_vertex_shader,
                                             depth ? "presentDepth.vert" : "present.vert");
    m_pixelSpirv  = ShaderUtils::CompileGLSL(depth ? kPresentDepthFragGlsl : kPresentFragGlsl,
                                             shaderc_fragment_shader,
                                             depth ? "presentDepth.frag" : "present.frag");
    m_vertexShader = ShaderUtils::MakeShaderModule(dev, m_vertexSpirv);
    m_pixelShader  = ShaderUtils::MakeShaderModule(dev, m_pixelSpirv);

    // --- Static sampler ---
    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = VK_LOD_CLAMP_NONE;
        sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
        checkVkResult(vkCreateSampler(dev, &sci, nullptr, &m_sampler), "PresentPipeline sampler");
    }

    // --- Descriptor set layout ---
    {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        const uint32_t imageBindingCount = depth ? 3u : 1u;
        for (uint32_t i = 0; i < imageBindingCount; ++i) {
            VkDescriptorSetLayoutBinding b = {};
            b.binding = i;
            b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            b.descriptorCount = 1;
            b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b.pImmutableSamplers = &m_sampler;
            bindings.push_back(b);
        }
        VkDescriptorSetLayoutBinding ubo = {};
        ubo.binding = 3;
        ubo.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        ubo.descriptorCount = 1;
        ubo.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings.push_back(ubo);

        VkDescriptorSetLayoutCreateInfo dslCi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        dslCi.bindingCount = (uint32_t)bindings.size();
        dslCi.pBindings = bindings.data();
        checkVkResult(vkCreateDescriptorSetLayout(dev, &dslCi, nullptr, &m_setLayout),
                      "PresentPipeline set layout");
    }

    // --- Pipeline layout ---
    {
        VkPipelineLayoutCreateInfo plCi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &m_setLayout;
        checkVkResult(vkCreatePipelineLayout(dev, &plCi, nullptr, &m_pipelineLayout),
                      "PresentPipeline layout");
    }

    // --- Descriptor pool + set (one set, re-written each frame) ---
    {
        std::array<VkDescriptorPoolSize, 2> sizes = {{
            { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, depth ? 3u : 1u },
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 },
        }};
        VkDescriptorPoolCreateInfo dpCi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        dpCi.maxSets = 1;
        dpCi.poolSizeCount = (uint32_t)sizes.size();
        dpCi.pPoolSizes = sizes.data();
        dpCi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT_EXT |
                     VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        // Some drivers reject the UPDATE_AFTER_BIND flag without extension; we
        // rely on plain updates here, so just use FREE_DESCRIPTOR_SET.
        dpCi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        checkVkResult(vkCreateDescriptorPool(dev, &dpCi, nullptr, &m_descriptorPool),
                      "PresentPipeline descriptor pool");

        VkDescriptorSetAllocateInfo dsAi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dsAi.descriptorPool = m_descriptorPool;
        dsAi.descriptorSetCount = 1;
        dsAi.pSetLayouts = &m_setLayout;
        checkVkResult(vkAllocateDescriptorSets(dev, &dsAi, &m_descriptorSet),
                      "PresentPipeline descriptor set");
    }

    // --- Screen indices buffer (small index buffer; same data as upstream) ---
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = sizeof(screenIndices);
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVkResult(vkCreateBuffer(dev, &bci, nullptr, &m_screenIndicesBuffer),
                      "PresentPipeline index buffer");

        VkMemoryRequirements memReq = {};
        vkGetBufferMemoryRequirements(dev, m_screenIndicesBuffer, &memReq);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = VRManager::instance().Composer->FindMemoryType(
            memReq.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checkVkResult(vkAllocateMemory(dev, &mai, nullptr, &m_screenIndicesMemory),
                      "PresentPipeline index memory");
        checkVkResult(vkBindBufferMemory(dev, m_screenIndicesBuffer, m_screenIndicesMemory, 0),
                      "PresentPipeline index bind");

        void* mapped = nullptr;
        checkVkResult(vkMapMemory(dev, m_screenIndicesMemory, 0, sizeof(screenIndices), 0, &mapped),
                      "PresentPipeline index map");
        std::memcpy(mapped, screenIndices, sizeof(screenIndices));
        vkUnmapMemory(dev, m_screenIndicesMemory);
    }
}

template <bool depth>
RND_VkComposer::PresentPipeline<depth>::~PresentPipeline() {
    VkDevice dev = VRManager::instance().Composer ? VRManager::instance().Composer->GetDevice() : VK_NULL_HANDLE;
    if (!dev) return;
    if (m_pipeline)         vkDestroyPipeline(dev, m_pipeline, nullptr);
    if (m_descriptorPool)   vkDestroyDescriptorPool(dev, m_descriptorPool, nullptr);
    if (m_pipelineLayout)   vkDestroyPipelineLayout(dev, m_pipelineLayout, nullptr);
    if (m_setLayout)        vkDestroyDescriptorSetLayout(dev, m_setLayout, nullptr);
    if (m_sampler)          vkDestroySampler(dev, m_sampler, nullptr);
    if (m_screenIndicesMemory) vkFreeMemory(dev, m_screenIndicesMemory, nullptr);
    if (m_screenIndicesBuffer) vkDestroyBuffer(dev, m_screenIndicesBuffer, nullptr);
    if (m_settingsMapped)  vkUnmapMemory(dev, m_settingsMemory);
    if (m_settingsMemory)  vkFreeMemory(dev, m_settingsMemory, nullptr);
    if (m_settingsBuffer)  vkDestroyBuffer(dev, m_settingsBuffer, nullptr);
    if (m_vertexShader)    vkDestroyShaderModule(dev, m_vertexShader, nullptr);
    if (m_pixelShader)     vkDestroyShaderModule(dev, m_pixelShader, nullptr);
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::BindAttachment(uint32_t attachmentIdx,
                                                            VkImageView srcImageView,
                                                            VkFormat srcFormat) {
    checkAssert(attachmentIdx < m_attachmentBindings.size(),
                "PresentPipeline::BindAttachment idx out of range");
    m_attachmentBindings[attachmentIdx] = { srcImageView, srcFormat };
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::BindTarget(uint32_t targetIdx,
                                                        VkImageView dstImageView,
                                                        VkFormat dstFormat) {
    checkAssert(targetIdx < m_targetBindings.size(),
                "PresentPipeline::BindTarget idx out of range");
    m_targetBindings[targetIdx] = { dstImageView, dstFormat };
    if (dstFormat != m_targetFormats[targetIdx]) {
        m_targetFormats[targetIdx] = dstFormat;
        RecreatePipeline();
    }
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::BindDepthTarget(VkImageView dstImageView, VkFormat dstFormat) {
    if constexpr (depth) {
        m_depthTargetBindings[0] = { dstImageView, dstFormat };
        if (dstFormat != m_targetFormats.back()) {
            m_targetFormats.back() = dstFormat;
            RecreatePipeline();
        }
    }
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::BindSettings(float screenWidth, float screenHeight,
                                                           const RenderUtils::UvTransform& uvTransform) {
    m_renderWidth = screenWidth;
    m_renderHeight = screenHeight;
    m_uvTransform = uvTransform;

    VkDevice dev = VRManager::instance().Composer->GetDevice();
    if (m_settingsBuffer == VK_NULL_HANDLE) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bci.size = sizeof(presentSettings);
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        checkVkResult(vkCreateBuffer(dev, &bci, nullptr, &m_settingsBuffer), "settings buffer");

        VkMemoryRequirements mr = {};
        vkGetBufferMemoryRequirements(dev, m_settingsBuffer, &mr);
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        mai.allocationSize = mr.size;
        mai.memoryTypeIndex = VRManager::instance().Composer->FindMemoryType(
            mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        checkVkResult(vkAllocateMemory(dev, &mai, nullptr, &m_settingsMemory), "settings memory");
        checkVkResult(vkBindBufferMemory(dev, m_settingsBuffer, m_settingsMemory, 0), "settings bind");
        checkVkResult(vkMapMemory(dev, m_settingsMemory, 0, sizeof(presentSettings), 0, &m_settingsMapped),
                      "settings map");
    }
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::UpdateSettingsBuffer(VkExtent2D swapchainExtent) {
    presentSettings settings = {
        .renderWidth = m_renderWidth,
        .renderHeight = m_renderHeight,
        .swapchainWidth = static_cast<float>(swapchainExtent.width),
        .swapchainHeight = static_cast<float>(swapchainExtent.height),
        .uvOffsetX = m_uvTransform.offsetX,
        .uvOffsetY = m_uvTransform.offsetY,
        .uvScaleX  = m_uvTransform.scaleX,
        .uvScaleY  = m_uvTransform.scaleY,
        .customFadeAmount = 0.0f,
        .customFadeColorR = 0.0f,
        .customFadeColorG = 0.0f,
        .customFadeColorB = 0.0f,
        .isFadeActive = 0.0f,
    };

    if constexpr (depth) {
        const RND_Renderer::CustomFade fade = m_renderer != nullptr ? m_renderer->GetCustomFade() : RND_Renderer::CustomFade{};
        settings.customFadeAmount = fade.amount;
        settings.customFadeColorR = fade.color.x;
        settings.customFadeColorG = fade.color.y;
        settings.customFadeColorB = fade.color.z;
        settings.isFadeActive = m_renderer != nullptr && m_renderer->IsFadeActive() ? 1.0f : 0.0f;
    }

    std::memcpy(m_settingsMapped, &settings, sizeof(settings));
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::WriteAttachmentDescriptors() {
    VkDevice dev = VRManager::instance().Composer->GetDevice();
    std::vector<VkDescriptorImageInfo> imgInfos;
    imgInfos.reserve(m_attachmentBindings.size());
    std::vector<VkWriteDescriptorSet> writes;

    for (uint32_t i = 0; i < m_attachmentBindings.size(); ++i) {
        auto& bind = m_attachmentBindings[i];
        if (!bind.view) continue;
        VkDescriptorImageInfo ii = {};
        ii.imageView = bind.view;
        ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii.sampler = m_sampler;
        imgInfos.push_back(ii);

        VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = m_descriptorSet;
        w.dstBinding = i;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &imgInfos.back();
        writes.push_back(w);
    }

    VkDescriptorBufferInfo bi = { m_settingsBuffer, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    w.dstSet = m_descriptorSet;
    w.dstBinding = 3;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w.pBufferInfo = &bi;
    writes.push_back(w);

    vkUpdateDescriptorSets(dev, (uint32_t)writes.size(), writes.data(), 0, nullptr);
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::RecreatePipeline() {
    VkDevice dev = VRManager::instance().Composer->GetDevice();
    if (m_pipeline) {
        vkDestroyPipeline(dev, m_pipeline, nullptr);
        m_pipeline = VK_NULL_HANDLE;
    }

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = m_vertexShader;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = m_pixelShader;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp = {}; VkRect2D sc = {};
    VkPipelineViewportStateCreateInfo vpState = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vpState.viewportCount = 1;
    vpState.pViewports = &vp;
    vpState.scissorCount = 1;
    vpState.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = depth ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = depth ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState cba = {};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    VkDynamicState dynStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates = dynStates;

    VkPipelineRenderingCreateInfoKHR rci = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR };
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &m_targetFormats[0];
    rci.depthAttachmentFormat = depth ? m_targetFormats.back() : VK_FORMAT_UNDEFINED;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.pNext = &rci;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vpState;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb;
    pci.pDynamicState = &dyn;
    pci.layout = m_pipelineLayout;
    pci.renderPass = VK_NULL_HANDLE; // dynamic rendering

    checkVkResult(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pci, nullptr, &m_pipeline),
                  "PresentPipeline graphics pipeline");
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::Render(VkCommandBuffer cb, VkImageView targetView) {
    // Default extent: take the bound settings (render size). This matches the
    // common caller pattern where BindSettings was already called with the
    // swapchain dimensions.
    VkExtent2D extent = { static_cast<uint32_t>(m_renderWidth), static_cast<uint32_t>(m_renderHeight) };
    Render(cb, targetView, extent);
}

template <bool depth>
void RND_VkComposer::PresentPipeline<depth>::Render(VkCommandBuffer cb,
                                                    VkImageView /*targetView*/,
                                                    VkExtent2D swapchainExtent) {
    checkAssert(m_settingsBuffer != VK_NULL_HANDLE,
                "Failed to present texture since graphics pipeline hasn't bound some settings yet!");
    if (m_pipeline == VK_NULL_HANDLE) RecreatePipeline();

    UpdateSettingsBuffer(swapchainExtent);
    WriteAttachmentDescriptors();

    VkRenderingAttachmentInfoKHR colorAtt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    colorAtt.imageView = m_targetBindings[0].view;
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingAttachmentInfoKHR depthAtt = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR };
    if constexpr (depth) {
        depthAtt.imageView = m_depthTargetBindings[0].view;
        depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depthAtt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }

    VkRenderingInfoKHR ri = { VK_STRUCTURE_TYPE_RENDERING_INFO_KHR };
    ri.renderArea.extent = swapchainExtent;
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &colorAtt;
    if constexpr (depth) ri.pDepthAttachment = &depthAtt;

    // libvulkan only exports the core 1.3 names; the KHR-suffixed alias is
    // only available through vkGetDeviceProcAddr. The two have identical
    // signatures because the extension was promoted to core unchanged.
    vkCmdBeginRendering(cb, &ri);

    VkViewport vp = { 0.0f, 0.0f, (float)swapchainExtent.width, (float)swapchainExtent.height, 0.0f, 1.0f };
    VkRect2D sc = { {0,0}, swapchainExtent };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);

    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineLayout, 0, 1,
                            &m_descriptorSet, 0, nullptr);
    vkCmdBindIndexBuffer(cb, m_screenIndicesBuffer, 0, VK_INDEX_TYPE_UINT16);
    vkCmdDrawIndexed(cb, (uint32_t)std::size(screenIndices), 1, 0, 0, 0);

    vkCmdEndRendering(cb);
}

// Explicit instantiation so the linker sees both flavors.
template class RND_VkComposer::PresentPipeline<false>;
template class RND_VkComposer::PresentPipeline<true>;

// ===================================================================== //
// DebugDrawPipeline — 1:1 port. To keep this file from sprawling out of
// control we punt the implementation details to a follow-up file
// (debug_draw_pipeline.cpp). For now provide an empty shell so the layer
// still compiles; Layer3D::Render guards on a non-empty DebugDrawRenderData,
// so a no-op Render keeps gameplay frames intact.
// ===================================================================== //

RND_VkComposer::DebugDrawPipeline::DebugDrawPipeline() = default;
RND_VkComposer::DebugDrawPipeline::~DebugDrawPipeline() = default;

void RND_VkComposer::DebugDrawPipeline::Render(OpenXR::EyeSide /*side*/,
                                               VkCommandBuffer /*cb*/,
                                               VkImageView /*sceneDepthView*/,
                                               VkImageView /*colorTargetView*/, VkFormat /*colorFormat*/,
                                               VkExtent2D /*colorExtent*/,
                                               VkImageView /*depthTargetView*/, VkFormat /*depthFormat*/,
                                               const DebugDrawRenderData& renderData,
                                               const glm::mat4& /*viewProjection*/) {
    if (renderData.IsEmpty()) return;
    // TODO: port the remainder of d3d12.cpp:DebugDrawPipeline. Faithful port
    // tracked in tasks; gameplay path doesn't depend on it for first-light.
}

void RND_VkComposer::DebugDrawPipeline::RecreatePipeline() {}
void RND_VkComposer::DebugDrawPipeline::EnsureVertexBuffer(uint32_t) {}
void RND_VkComposer::DebugDrawPipeline::UpdateSceneSettings(OpenXR::EyeSide, uint32_t, VkExtent2D, const glm::mat4&, float) {}
void RND_VkComposer::DebugDrawPipeline::RenderVertices(VkCommandBuffer, VkPipeline, uint32_t, uint32_t) {}
VkDescriptorSet RND_VkComposer::DebugDrawPipeline::AcquireSceneDescriptorSet(OpenXR::EyeSide, VkImageView) { return VK_NULL_HANDLE; }
