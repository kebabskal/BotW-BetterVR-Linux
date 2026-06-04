#include "pch.h"

#include "swapchain.h"
#include "instance.h"
#include "utils/vulkan_utils.h"

// Templated VR swapchain. Mirrors upstream d3d12 version's lifecycle:
//   ctor → xrCreateSwapchain + enumerate VkImage[] + create VkImageView per
//   PrepareRendering → no-op (D3D12 used this for sync; we don't need it)
//   StartRendering    → xrAcquireSwapchainImage + xrWaitSwapchainImage
//   FinishRendering   → xrReleaseSwapchainImage

namespace {
    // Pull the requested format from the XR-side enumerated list. Quest 2 via
    // WiVRn typically lists ~13 formats; if our preferred format isn't
    // available, fall back to the runtime's first choice (recommended).
    int64_t PickFormat(const std::vector<int64_t>& formats, VkFormat preferred) {
        for (int64_t f : formats) if (f == (int64_t)preferred) return f;
        return formats.empty() ? 0 : formats.front();
    }
}

template <VkFormat T>
Swapchain<T>::Swapchain(uint32_t width, uint32_t height, uint32_t sampleCount)
    : m_width(width), m_height(height)
{
    XrSession session = VRManager::instance().XR->GetSession();
    checkAssert(session != XR_NULL_HANDLE, "Swapchain ctor: no XR session yet");

    uint32_t formatCount = 0;
    checkXRResult(xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr),
                  "xrEnumerateSwapchainFormats count");
    std::vector<int64_t> formats(formatCount);
    checkXRResult(xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data()),
                  "xrEnumerateSwapchainFormats");
    const int64_t chosen = PickFormat(formats, T);
    m_format = static_cast<VkFormat>(chosen);

    XrSwapchainCreateInfo sci = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
    sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                     XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    if constexpr (T == VK_FORMAT_D32_SFLOAT || T == VK_FORMAT_D16_UNORM ||
                  T == VK_FORMAT_D24_UNORM_S8_UINT || T == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        sci.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                         XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    }
    sci.format = chosen;
    sci.sampleCount = sampleCount;
    sci.width = width;
    sci.height = height;
    sci.faceCount = 1;
    sci.arraySize = 1;
    sci.mipCount = 1;

    checkXRResult(xrCreateSwapchain(session, &sci, &m_swapchain), "xrCreateSwapchain");

    uint32_t imageCount = 0;
    checkXRResult(xrEnumerateSwapchainImages(m_swapchain, 0, &imageCount, nullptr),
                  "xrEnumerateSwapchainImages count");
    std::vector<XrSwapchainImageVulkan2KHR> xrImages(imageCount, { XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR });
    checkXRResult(xrEnumerateSwapchainImages(m_swapchain, imageCount, &imageCount,
                                             reinterpret_cast<XrSwapchainImageBaseHeader*>(xrImages.data())),
                  "xrEnumerateSwapchainImages");

    m_swapchainImages.reserve(imageCount);
    m_swapchainImageViews.reserve(imageCount);
    VkDevice dev = VRManager::instance().Composer->GetDevice();
    for (auto& xi : xrImages) {
        m_swapchainImages.push_back(xi.image);
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vci.image = xi.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = m_format;
        vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                           VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
        vci.subresourceRange.aspectMask = VulkanUtils::GetAspectMaskForFormat(m_format);
        vci.subresourceRange.levelCount = 1;
        vci.subresourceRange.layerCount = 1;
        VkImageView view = VK_NULL_HANDLE;
        checkVkResult(vkCreateImageView(dev, &vci, nullptr, &view), "xr swapchain image view");
        m_swapchainImageViews.push_back(view);
    }
}

template <VkFormat T>
Swapchain<T>::~Swapchain() {
    VkDevice dev = VRManager::instance().Composer ? VRManager::instance().Composer->GetDevice() : VK_NULL_HANDLE;
    if (dev) {
        for (VkImageView v : m_swapchainImageViews) if (v) vkDestroyImageView(dev, v, nullptr);
    }
    if (m_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(m_swapchain);
}

template <VkFormat T>
void Swapchain<T>::PrepareRendering() {
    // D3D12 needed an explicit "prepare" step to advance fence values; Vulkan
    // acquire+wait in StartRendering does the equivalent.
}

template <VkFormat T>
VkImage Swapchain<T>::StartRendering() {
    XrSwapchainImageAcquireInfo ai = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
    checkXRResult(xrAcquireSwapchainImage(m_swapchain, &ai, &m_swapchainImageIdx),
                  "xrAcquireSwapchainImage");
    XrSwapchainImageWaitInfo wi = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
    wi.timeout = XR_INFINITE_DURATION;
    checkXRResult(xrWaitSwapchainImage(m_swapchain, &wi), "xrWaitSwapchainImage");
    return m_swapchainImages[m_swapchainImageIdx];
}

template <VkFormat T>
void Swapchain<T>::FinishRendering() {
    XrSwapchainImageReleaseInfo ri = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
    checkXRResult(xrReleaseSwapchainImage(m_swapchain, &ri), "xrReleaseSwapchainImage");
}

// Explicit instantiations to match every Layer3D/Layer2D template usage.
template class Swapchain<VK_FORMAT_R8G8B8A8_SRGB>;
template class Swapchain<VK_FORMAT_D32_SFLOAT>;
