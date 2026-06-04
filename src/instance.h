#pragma once

#include <pch.h>

#include "hooking/cemu_hooks.h"
#include "rendering/vk_composer.h"
#include "rendering/openxr.h"
#include "rendering/renderer.h"
#include "rendering/vulkan.h"

class VRManager {
public:
    static VRManager& instance() {
        static VRManager singletonInstance;
        return singletonInstance;
    }

    VRManager(VRManager const&) = delete;
    void operator=(VRManager const&) = delete;

    // Cemu-side init: called from the vkroots layer once Cemu has created its
    // own VkInstance + VkDevice. Composer is the separate Vulkan stack that
    // submits VR frames; it owns its own VkInstance + VkDevice.
    void Init(VkInstance cemuInstance, VkPhysicalDevice cemuPhysicalDevice, VkDevice cemuDevice) {
        Composer = std::make_unique<RND_VkComposer>();
        VK = std::make_unique<RND_Vulkan>(cemuInstance, cemuPhysicalDevice, cemuDevice);
        Log::print<INFO>("Initialized VRManager instance...");
    }

    void InitSession() {
        // Use the Vulkan2 graphics binding. The values come from RND_VkComposer
        // (its VkInstance / VkPhysicalDevice / VkDevice / VkQueue were created
        // through xrCreateVulkan{Instance,Device}KHR so they're guaranteed
        // compatible with the runtime).
        XrGraphicsBindingVulkan2KHR vkBinding = { XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR };
        vkBinding.instance = Composer->GetInstance();
        vkBinding.physicalDevice = Composer->GetPhysicalDevice();
        vkBinding.device = Composer->GetDevice();
        vkBinding.queueFamilyIndex = Composer->GetQueueFamilyIndex();
        vkBinding.queueIndex = 0;
        XR->CreateSession(vkBinding);
        XR->CreateActions();
        Hooks = std::make_unique<CemuHooks>();
    }

    std::unique_ptr<OpenXR> XR;
    std::unique_ptr<RND_VkComposer> Composer;
    std::unique_ptr<RND_Vulkan> VK;
    std::unique_ptr<CemuHooks> Hooks;

    uint32_t vkVersion = 0;

private:
    VRManager() {
        m_logger = std::make_unique<Log>();
        XR = std::make_unique<OpenXR>();
    }

    ~VRManager() {
        // Order: OpenXR must drop swapchains before we tear down the composer.
        VK.reset();
        XR.reset();
        Composer.reset();
        m_logger.reset();
    }

    std::unique_ptr<Log> m_logger;
};
