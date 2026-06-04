#pragma once

// Linux Vulkan-port texture hierarchy. Preserves upstream's three-layer split:
//
//   BaseVulkanTexture    — image on Cemu's VkDevice  (already Vulkan upstream)
//     └ VulkanTexture     — derived: + VkImageView
//         └ VulkanFramebuffer — derived: + VkFramebuffer
//
//   Texture              — image on the VR Composer's VkDevice
//                          (was D3D12 on Windows; now plain Vulkan)
//
//   SharedTexture        — inherits from both: same memory backs a VkImage on
//                          Cemu's device and an imported VkImage on the Composer's
//                          device, via VK_KHR_external_memory_fd. Sync via
//                          VK_KHR_external_semaphore_fd timeline semaphores.
//
// Cross-device sharing (Linux equivalent of D3D12 NT-shared HANDLE):
//   1. Cemu-side device allocates VkDeviceMemory with VkExportMemoryAllocateInfo
//      {OPAQUE_FD}, creates a VkImage on it, and binds.
//   2. vkGetMemoryFdKHR returns an int file descriptor.
//   3. Composer-side device imports the fd via VkImportMemoryFdInfoKHR, gets
//      a VkDeviceMemory handle, creates a VkImage with the same metadata, and
//      binds. The two VkImage handles alias the same physical pages.
//   4. Same pattern for VkSemaphore via vkGetSemaphoreFdKHR /
//      VkImportSemaphoreFdInfoKHR. Timeline semantics work across the import.

class SharedTexture;

// ===================================================================== //
// BaseVulkanTexture — Cemu-side. (Unchanged from upstream.)
// ===================================================================== //
class BaseVulkanTexture {
    friend class SharedTexture;
    friend class VulkanTexture;
public:
    BaseVulkanTexture(uint32_t width, uint32_t height, VkFormat vkFormat)
        : m_width(width), m_height(height), m_vkFormat(vkFormat) {}
    virtual ~BaseVulkanTexture();

    void vkPipelineBarrier(VkCommandBuffer cmdBuffer);
    void vkTransitionLayout(VkCommandBuffer cmdBuffer, VkImageLayout newLayout);

    void vkClear(VkCommandBuffer cmdBuffer, VkClearColorValue color);
    void vkClearDepth(VkCommandBuffer cmdBuffer, float depth, uint32_t stencil = 0);
    void vkCopyToImage(VkCommandBuffer cmdBuffer, VkImage dstImage);
    // AMD GPU FIX: caller-known source layout, skips redundant transitions.
    void vkCopyFromImage(VkCommandBuffer cmdBuffer, VkImage srcImage);

    bool vkIsUploadingTexture() const { return isStagingUpload; }
    void vkUpload(VkCommandBuffer cmdBuffer, const void* data, size_t size);
    void vkTryToFinishAnyUploads(VkCommandBuffer cmdBuffer);

    uint32_t GetWidth() const { return m_width; }
    uint32_t GetHeight() const { return m_height; }
    VkFormat GetFormat() const { return m_vkFormat; }
    VkImageAspectFlags GetAspectMask() const;
    VkImage GetImage() const { return m_vkImage; }

protected:
    VkImage m_vkImage = VK_NULL_HANDLE;
    VkDeviceMemory m_vkMemory = VK_NULL_HANDLE;
    VkImageLayout m_vkCurrLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t m_width;
    uint32_t m_height;
    VkFormat m_vkFormat;

    bool isStagingUpload = false;
    VkCommandBuffer m_uploadCommandBuffer = VK_NULL_HANDLE;
    VkBuffer m_stagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_stagingMemory = VK_NULL_HANDLE;
};

class VulkanTexture : public BaseVulkanTexture {
    friend class VulkanFramebuffer;
public:
    VulkanTexture(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, bool disableAlphaThroughSwizzling);
    VulkanTexture(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage)
        : VulkanTexture(width, height, format, usage, false) {}
    ~VulkanTexture() override;

    VkImageView GetImageView() const { return m_vkImageView; }

private:
    VkImageView m_vkImageView = VK_NULL_HANDLE;
};

class VulkanFramebuffer : public VulkanTexture {
public:
    VulkanFramebuffer(uint32_t width, uint32_t height, VkFormat format, VkRenderPass renderPass);
    ~VulkanFramebuffer() override;

    VkFramebuffer GetFramebuffer() const { return m_framebuffer; }
private:
    VkFramebuffer m_framebuffer = VK_NULL_HANDLE;
};

// ===================================================================== //
// Texture — composer-side (was D3D12; now Vulkan on the Composer's VkDevice).
// ===================================================================== //
class Texture {
public:
    Texture(uint32_t width, uint32_t height, VkFormat format);
    virtual ~Texture();

    // Composer-side timeline sync. The methods below replace upstream's
    // d3d12SignalFence/d3d12WaitForFence. Submission with timeline semaphores
    // happens via the CommandContext / vkQueueSubmit path, so these helpers
    // only update the tracker state (used for double-signal warnings).
    void TrackSignaledValue(uint64_t value);
    void TrackWaitedValue(uint64_t value);

    // Layout helper (replaces d3d12TransitionLayout). Issues a single image
    // barrier on `cmdList` to transition the composer-side VkImage.
    void TransitionLayout(VkCommandBuffer cmdList, VkImageLayout newLayout);

    VkImage          GetComposerImage() const { return m_composerImage; }
    VkImageView      GetComposerImageView() const { return m_composerImageView; }
    VkFormat         GetFormat() const { return m_composerFormat; }
    VkSemaphore      GetTimelineSemaphore() const { return m_timelineSemaphore; }

    uint64_t GetLastSignalledValue() const { return m_fenceLastSignaledValue; }
    uint64_t GetLastAwaitedValue() const { return m_fenceLastAwaitedValue; }

protected:
    void SetLastSignalledValue(uint64_t value);
    void SetLastAwaitedValue(uint64_t value);

    // Composer-side VkImage + memory + view. For SharedTexture these are
    // imported from the Cemu side via opaque-fd.
    VkFormat       m_composerFormat = VK_FORMAT_UNDEFINED;
    VkImage        m_composerImage = VK_NULL_HANDLE;
    VkDeviceMemory m_composerMemory = VK_NULL_HANDLE;
    VkImageView    m_composerImageView = VK_NULL_HANDLE;
    VkImageLayout  m_composerLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // Timeline semaphore shared with the Cemu-side device via opaque-fd.
    // Used for cross-device GPU sync (replaces D3D12 fence + HANDLE).
    VkSemaphore m_timelineSemaphore = VK_NULL_HANDLE;
    uint64_t m_fenceLastSignaledValue = 0;
    uint64_t m_fenceLastAwaitedValue = 0;
};

// ===================================================================== //
// SharedTexture — dual identity, single backing memory + semaphore.
// ===================================================================== //
class SharedTexture : public Texture, public BaseVulkanTexture {
public:
    SharedTexture(uint32_t width, uint32_t height, VkFormat vkFormat, VkFormat composerFormat);
    ~SharedTexture() override;

    // Two-phase init: SharedTexture ctor allocates nothing; the first call
    // to Init records the actual VkImage allocations (on the Cemu-side
    // VkCommandBuffer for layout transitions) and shares the memory across.
    void Init(const VkCommandBuffer& cmdBuffer);

    // Copy a Cemu-side VkImage's pixels into the shared backing. Records
    // commands on `cmdBuffer` (Cemu's command buffer). Matches upstream API.
    void CopyFromVkImage(VkCommandBuffer cmdBuffer, VkImage srcImage);

    // Composer-side timeline semaphore handle (created on composer device).
    const VkSemaphore& GetSemaphore() const { return m_timelineSemaphore; }
    const VkSemaphore& GetComposerSemaphore() const { return m_timelineSemaphore; }
    // Cemu-side timeline semaphore handle (imported from composer's fd; shares
    // state with m_timelineSemaphore but must be used for vkQueueSubmit on the
    // Cemu device).
    const VkSemaphore& GetCemuSemaphore() const { return m_cemuTimelineSemaphore; }

    // AMD GPU FIX: monotonic counter (same convention as upstream — preserved
    // here so both sides agree on parity-based readiness checks in Layer2D).
    //   Vulkan waits for value N (last D3D12/composer signal, or 0 initially)
    //   Vulkan copies, then signals N+1
    //   Composer waits for N+1
    //   Composer uses texture, then signals N+2
    // (Odd values are Cemu-side signals, even values are composer-side signals.)
    uint64_t GetVulkanWaitValue() const  { return m_fenceCounter.load(); }
    uint64_t GetVulkanSignalValue()       { return ++m_fenceCounter; }
    uint64_t GetD3D12WaitValue() const   { return m_fenceCounter.load(); }
    uint64_t GetD3D12SignalValue()        { return ++m_fenceCounter; }

    // Called from Cemu's vkQueueSubmit path (see framebuffer.cpp). Must return
    // the Cemu-device handle, not the composer's.
    const VkSemaphore& GetSemaphoreForSignal(uint64_t dbgSignalTo = 0) {
        SetLastSignalledValue(dbgSignalTo);
        return m_cemuTimelineSemaphore;
    }
    const VkSemaphore& GetSemaphoreForWait(uint64_t dbgWaitFor = 0) {
        SetLastAwaitedValue(dbgWaitFor);
        return m_cemuTimelineSemaphore;
    }

private:
    // Cross-device sharing handles (Linux: file descriptors, ownership Linux:
    // composer-side import owns the fd-derived VkDeviceMemory / VkSemaphore;
    // Cemu-side owns the exporting originals).
    int m_memoryFd = -1;
    int m_semaphoreFd = -1;

    // Cemu-side timeline semaphore — imported from the composer's exported
    // fd. Shares state with m_timelineSemaphore (in BaseVulkanTexture).
    VkSemaphore m_cemuTimelineSemaphore = VK_NULL_HANDLE;

    std::atomic_bool m_activeOperation = false;
    std::atomic<uint64_t> m_fenceCounter{ 0 };
};
