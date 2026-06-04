#pragma once

// Templated OpenXR swapchain wrapper. Upstream templated this on
// DXGI_FORMAT (D3D12 side); on Linux we template on VkFormat because the
// composer is Vulkan. The XrSwapchain itself is graphics-API-agnostic — the
// binding type chosen at session creation determines whether the underlying
// images are VkImage or ID3D12Resource.
template <VkFormat T>
class Swapchain {
public:
    Swapchain(uint32_t width, uint32_t height, uint32_t sampleCount);
    ~Swapchain();

    // PrepareRendering: stamp the current acquire-image index so subsequent
    // StartRendering returns the same texture (matches upstream).
    void PrepareRendering();
    VkImage StartRendering();
    void FinishRendering();

    XrSwapchain GetHandle() const { return m_swapchain; }
    VkImage     GetTexture() const { return m_swapchainImages[m_swapchainImageIdx]; }
    VkImageView GetTextureView() const { return m_swapchainImageViews[m_swapchainImageIdx]; }

    VkFormat GetFormat() const { return m_format; }
    [[nodiscard]] uint32_t GetWidth() const { return m_width; }
    [[nodiscard]] uint32_t GetHeight() const { return m_height; }
    [[nodiscard]] VkExtent2D GetExtent() const { return { m_width, m_height }; }

private:
    XrSwapchain m_swapchain = XR_NULL_HANDLE;
    uint32_t m_width;
    uint32_t m_height;
    VkFormat m_format = T;

    std::vector<VkImage> m_swapchainImages;
    std::vector<VkImageView> m_swapchainImageViews;
    uint32_t m_swapchainImageIdx = 0;
};
