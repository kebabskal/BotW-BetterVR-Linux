#include "vulkan.h"
#include "hooking/layer.h"
#include "instance.h"
#include "utils/logger.h"

// Defined at global scope in hooking/framebuffer.cpp. Declared at file scope
// here (not inside a VRLayer::Method body) so the symbol resolves globally —
// otherwise the unqualified declaration would be interpreted as a member of
// the enclosing namespace and the loader would reject our .so with an
// undefined VRLayer::RegisterCemuSwapchain symbol.
void RegisterCemuSwapchain(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device,
                           VkSwapchainKHR swapchain, const VkSwapchainCreateInfoKHR* pCreateInfo);

RND_Vulkan::RND_Vulkan(VkInstance vkInstance, VkPhysicalDevice vkPhysDevice, VkDevice vkDevice): m_instance(vkInstance), m_physicalDevice(vkPhysDevice), m_device(vkDevice) {
    m_instanceDispatch = vkroots::tables::InstanceDispatches.find(vkInstance);
    m_physicalDeviceDispatch = vkroots::tables::PhysicalDeviceDispatches.find(vkPhysDevice);
    m_deviceDispatch = vkroots::tables::DeviceDispatches.find(vkDevice);

    // AMD GPU FIX: Initialize sType before calling vkGetPhysicalDeviceMemoryProperties2
    m_memoryProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    m_memoryProperties.pNext = nullptr;
    m_physicalDeviceDispatch->GetPhysicalDeviceMemoryProperties2KHR(vkPhysDevice, &m_memoryProperties);

    VkPhysicalDeviceProperties props{};
    m_instanceDispatch->GetPhysicalDeviceProperties(vkPhysDevice, &props);

    uint64_t localVramBytes = 0;
    for (uint32_t i = 0; i < m_memoryProperties.memoryProperties.memoryHeapCount; ++i) {
        if ((m_memoryProperties.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0) {
            localVramBytes += m_memoryProperties.memoryProperties.memoryHeaps[i].size;
        }
    }

    Log::print<INFO>("GPU: {} (vendor={:#06x}, device={:#06x}, driver={})", props.deviceName, props.vendorID, props.deviceID, props.driverVersion);
    if (localVramBytes > 0) {
        Log::print<INFO>("GPU VRAM (device local): {:.2f} GiB", double(localVramBytes) / (1024.0 * 1024.0 * 1024.0));
    }
}

RND_Vulkan::~RND_Vulkan() {
}

uint32_t RND_Vulkan::FindMemoryType(uint32_t memoryTypeBitsRequirement, VkMemoryPropertyFlags requirementsMask) {
    // AMD GPU FIX: Use actual memoryTypeCount instead of VK_MAX_MEMORY_TYPES to avoid reading uninitialized data
    const uint32_t memoryTypeCount = m_memoryProperties.memoryProperties.memoryTypeCount;
    for (uint32_t i = 0; i < memoryTypeCount; i++) {
        const uint32_t memoryTypeBits = (1u << i);
        const bool isRequiredMemoryType = (memoryTypeBitsRequirement & memoryTypeBits) != 0;
        const bool satisfiesFlags = (m_memoryProperties.memoryProperties.memoryTypes[i].propertyFlags & requirementsMask) == requirementsMask;

        if (isRequiredMemoryType && satisfiesFlags) {
            return i;
        }
    }
    checkAssert(false, "Failed to find suitable memory type");
    return 0;
}


VkResult VRLayer::VkDeviceOverrides::GetPhysicalDeviceSurfacePresentModesKHR(const vkroots::VkDeviceDispatch& pDispatch, VkPhysicalDevice physicalDevice, VkSurfaceKHR surface, uint32_t* pPresentModeCount, VkPresentModeKHR* pPresentModes) {
    // check all supported present modes
    uint32_t testPresentModes = 0;
    VkResult result = pDispatch.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &testPresentModes, VK_NULL_HANDLE);

    std::vector<VkPresentModeKHR> supportedPresentModes;
    supportedPresentModes.resize(testPresentModes);

    result = pDispatch.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &testPresentModes, supportedPresentModes.data());

    checkAssert(result == VK_SUCCESS, "Failed to get physical device surface present modes!");

    // if VK_PRESENT_MODE_IMMEDIATE_KHR is present, always just return that
    for (uint32_t i = 0; i < testPresentModes; i++) {
        if (supportedPresentModes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) {
            // immediate is supported, only return 1 mode
            if (pPresentModes != VK_NULL_HANDLE) {
                pPresentModes[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
            }
            else {
                *pPresentModeCount = 1;
            }
        }
    }

    // otherwise, we just try and use whatever mode is available
    return pDispatch.GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, pPresentModeCount, pPresentModes);
}

VkResult VRLayer::VkDeviceOverrides::CreateSwapchainKHR(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain) {
    VkResult res = pDispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (res == VK_SUCCESS) {
        // Track Cemu's swapchains so the mirror in framebuffer.cpp can find
        // VkImages by swapchain handle + present-index in vkQueuePresentKHR.
        ::RegisterCemuSwapchain(pDispatch, device, *pSwapchain, pCreateInfo);
    }
    return res;
}