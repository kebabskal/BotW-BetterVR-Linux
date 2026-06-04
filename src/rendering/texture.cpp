#include "pch.h"

#include "texture.h"
#include "instance.h"
#include "utils/vulkan_utils.h"

#include <unistd.h>  // close(fd)

// ===================================================================== //
// BaseVulkanTexture — Cemu-side (unchanged from upstream, modulo the
// dispatch-table-vs-loader-prototype split: Cemu-side calls still go through
// vkroots dispatch because they live behind the layer interception, while
// composer-side calls in Texture/SharedTexture below use loader prototypes
// since the composer's VkDevice was created outside vkroots).
// ===================================================================== //

BaseVulkanTexture::~BaseVulkanTexture() {
    if (m_vkImage != VK_NULL_HANDLE) {
        VRManager::instance().VK->GetDeviceDispatch()->DestroyImage(VRManager::instance().VK->GetDevice(), m_vkImage, nullptr);
        m_vkImage = VK_NULL_HANDLE;
    }
    if (m_vkMemory != VK_NULL_HANDLE) {
        VRManager::instance().VK->GetDeviceDispatch()->FreeMemory(VRManager::instance().VK->GetDevice(), m_vkMemory, nullptr);
        m_vkMemory = VK_NULL_HANDLE;
    }
}

void BaseVulkanTexture::vkPipelineBarrier(VkCommandBuffer cmdBuffer) {
    return VulkanUtils::DebugPipelineBarrier(cmdBuffer);
}

VkImageAspectFlags BaseVulkanTexture::GetAspectMask() const {
    return VulkanUtils::GetAspectMaskForFormat(m_vkFormat);
}

void BaseVulkanTexture::vkTransitionLayout(VkCommandBuffer cmdBuffer, VkImageLayout newLayout) {
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    VulkanUtils::TransitionLayout(cmdBuffer, m_vkImage, m_vkCurrLayout, newLayout, GetAspectMask());
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    m_vkCurrLayout = newLayout;
}

void BaseVulkanTexture::vkCopyToImage(VkCommandBuffer cmdBuffer, VkImage dstImage) {
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkImageAspectFlags aspectMask = GetAspectMask();
    const VkImageCopy region = {
        .srcSubresource = { aspectMask, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { aspectMask, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { m_width, m_height, 1 }
    };
    dispatch->CmdCopyImage(cmdBuffer, m_vkImage, m_vkCurrLayout, dstImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    VkMemoryBarrier2 postCopyBarrier = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    postCopyBarrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    postCopyBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    postCopyBarrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    postCopyBarrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDependencyInfo depInfo = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    depInfo.memoryBarrierCount = 1;
    depInfo.pMemoryBarriers = &postCopyBarrier;
    dispatch->CmdPipelineBarrier2(cmdBuffer, &depInfo);
}

void BaseVulkanTexture::vkClear(VkCommandBuffer cmdBuffer, VkClearColorValue color) {
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    if (VulkanUtils::IsDepthFormat(m_vkFormat)) {
        Log::print<WARNING>("vkClear called on depth image - use vkClearDepth instead");
        return;
    }
    if (m_vkCurrLayout != VK_IMAGE_LAYOUT_GENERAL && m_vkCurrLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        VulkanUtils::TransitionLayout(cmdBuffer, m_vkImage, m_vkCurrLayout, VK_IMAGE_LAYOUT_GENERAL);
        m_vkCurrLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    const VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel = 0,
        .levelCount = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS
    };
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    dispatch->CmdClearColorImage(cmdBuffer, m_vkImage, m_vkCurrLayout, &color, 1, &range);
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
}

void BaseVulkanTexture::vkClearDepth(VkCommandBuffer cmdBuffer, float depth, uint32_t stencil) {
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    if (!VulkanUtils::IsDepthFormat(m_vkFormat)) {
        Log::print<WARNING>("vkClearDepth called on color image - use vkClear instead");
        return;
    }
    if (m_vkCurrLayout != VK_IMAGE_LAYOUT_GENERAL && m_vkCurrLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        VulkanUtils::TransitionLayout(cmdBuffer, m_vkImage, m_vkCurrLayout, VK_IMAGE_LAYOUT_GENERAL, GetAspectMask());
        m_vkCurrLayout = VK_IMAGE_LAYOUT_GENERAL;
    }
    VkClearDepthStencilValue clearValue = { .depth = depth, .stencil = stencil };
    const VkImageSubresourceRange range = {
        .aspectMask = GetAspectMask(),
        .baseMipLevel = 0,
        .levelCount = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS
    };
    dispatch->CmdClearDepthStencilImage(cmdBuffer, m_vkImage, m_vkCurrLayout, &clearValue, 1, &range);
}

void BaseVulkanTexture::vkCopyFromImage(VkCommandBuffer cmdBuffer, VkImage srcImage) {
    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkImageAspectFlags aspectMask = GetAspectMask();
    const VkImageCopy region = {
        .srcSubresource = { aspectMask, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { aspectMask, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { m_width, m_height, 1 }
    };
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    dispatch->CmdCopyImage(cmdBuffer, srcImage, VK_IMAGE_LAYOUT_GENERAL, m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
}

void BaseVulkanTexture::vkUpload(VkCommandBuffer cmdBuffer, const void* data, size_t size) {
    m_uploadCommandBuffer = cmdBuffer;
    isStagingUpload = true;

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkDevice device = VRManager::instance().VK->GetDevice();

    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    checkVkResult(dispatch->CreateBuffer(device, &bi, nullptr, &m_stagingBuffer), "staging buffer");

    VkMemoryRequirements mr;
    dispatch->GetBufferMemoryRequirements(device, m_stagingBuffer, &mr);

    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = VRManager::instance().VK->FindMemoryType(
        mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    checkVkResult(dispatch->AllocateMemory(device, &ai, nullptr, &m_stagingMemory), "staging memory");
    checkVkResult(dispatch->BindBufferMemory(device, m_stagingBuffer, m_stagingMemory, 0), "staging bind");

    void* mapped;
    checkVkResult(dispatch->MapMemory(device, m_stagingMemory, 0, size, 0, &mapped), "staging map");
    std::memcpy(mapped, data, size);
    dispatch->UnmapMemory(device, m_stagingMemory);

    VkBufferImageCopy region = {};
    region.imageSubresource.aspectMask = GetAspectMask();
    region.imageSubresource.layerCount = 1;
    region.imageExtent = { m_width, m_height, 1 };
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    dispatch->CmdCopyBufferToImage(cmdBuffer, m_stagingBuffer, m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
}

void BaseVulkanTexture::vkTryToFinishAnyUploads(VkCommandBuffer cmdBuffer) {
    if (isStagingUpload && m_uploadCommandBuffer != cmdBuffer) {
        isStagingUpload = false;
        m_uploadCommandBuffer = VK_NULL_HANDLE;
        auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
        VkDevice device = VRManager::instance().VK->GetDevice();
        if (m_stagingBuffer != VK_NULL_HANDLE) { dispatch->DestroyBuffer(device, m_stagingBuffer, nullptr); m_stagingBuffer = VK_NULL_HANDLE; }
        if (m_stagingMemory != VK_NULL_HANDLE) { dispatch->FreeMemory(device, m_stagingMemory, nullptr); m_stagingMemory = VK_NULL_HANDLE; }
    }
}

VulkanTexture::VulkanTexture(uint32_t width, uint32_t height, VkFormat format, VkImageUsageFlags usage, bool disableAlphaThroughSwizzling)
    : BaseVulkanTexture(width, height, format)
{
    const auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { m_width, m_height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVkResult(dispatch->CreateImage(VRManager::instance().VK->GetDevice(), &ici, nullptr, &m_vkImage), "VulkanTexture image");

    VkMemoryRequirements mr;
    dispatch->GetImageMemoryRequirements(VRManager::instance().VK->GetDevice(), m_vkImage, &mr);
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = VRManager::instance().VK->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVkResult(dispatch->AllocateMemory(VRManager::instance().VK->GetDevice(), &ai, nullptr, &m_vkMemory), "VulkanTexture memory");
    checkVkResult(dispatch->BindImageMemory(VRManager::instance().VK->GetDevice(), m_vkImage, m_vkMemory, 0), "VulkanTexture bind");

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = m_vkImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.components = {
        VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
        VK_COMPONENT_SWIZZLE_IDENTITY,
        disableAlphaThroughSwizzling ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    checkVkResult(dispatch->CreateImageView(VRManager::instance().VK->GetDevice(), &vci, nullptr, &m_vkImageView), "VulkanTexture view");
}

VulkanTexture::~VulkanTexture() {
    if (m_vkImageView != VK_NULL_HANDLE) {
        VRManager::instance().VK->GetDeviceDispatch()->DestroyImageView(VRManager::instance().VK->GetDevice(), m_vkImageView, nullptr);
        m_vkImageView = VK_NULL_HANDLE;
    }
}

VulkanFramebuffer::VulkanFramebuffer(uint32_t width, uint32_t height, VkFormat format, VkRenderPass renderPass)
    : VulkanTexture(width, height, format,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
{
    const auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkFramebufferCreateInfo fbi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    fbi.renderPass = renderPass;
    fbi.attachmentCount = 1;
    fbi.pAttachments = &m_vkImageView;
    fbi.width = width;
    fbi.height = height;
    fbi.layers = 1;
    checkVkResult(dispatch->CreateFramebuffer(VRManager::instance().VK->GetDevice(), &fbi, nullptr, &m_framebuffer), "VulkanFramebuffer");
}

VulkanFramebuffer::~VulkanFramebuffer() {
    if (m_framebuffer != VK_NULL_HANDLE)
        VRManager::instance().VK->GetDeviceDispatch()->DestroyFramebuffer(VRManager::instance().VK->GetDevice(), m_framebuffer, nullptr);
}

// ===================================================================== //
// Texture — composer-side (was D3D12-backed; now Vulkan on the Composer's
// VkDevice). Memory + semaphore are allocated with VkExport*Info so they can
// be shared into Cemu's device via opaque-fd. SharedTexture ctor does the
// Cemu-side import after Texture's ctor has produced the fds.
// ===================================================================== //

Texture::Texture(uint32_t width, uint32_t height, VkFormat format)
    : m_composerFormat(format)
{
    // Allocated by SharedTexture (uses m_composerFormat). A plain Texture
    // (non-shared) isn't used anywhere in the current renderer, but keep the
    // class buildable so the upstream class hierarchy is preserved.
}

Texture::~Texture() {
    VkDevice dev = VRManager::instance().Composer ? VRManager::instance().Composer->GetDevice() : VK_NULL_HANDLE;
    if (!dev) return;
    if (m_composerImageView) vkDestroyImageView(dev, m_composerImageView, nullptr);
    if (m_composerImage)     vkDestroyImage(dev, m_composerImage, nullptr);
    if (m_composerMemory)    vkFreeMemory(dev, m_composerMemory, nullptr);
    if (m_timelineSemaphore) vkDestroySemaphore(dev, m_timelineSemaphore, nullptr);
}

void Texture::SetLastSignalledValue(uint64_t value) {
    static uint32_t s_signalCount = 0;
    s_signalCount++;
    if (s_signalCount % 500 == 0 || m_fenceLastSignaledValue == value) {
        Log::print<INTEROP>("Semaphore signal #{}: texture={}, value {} -> {} (last waited={})",
                            s_signalCount, (void*)this, m_fenceLastSignaledValue, value, m_fenceLastAwaitedValue);
    }
    if (m_fenceLastSignaledValue == value && value != 0) {
        Log::print<WARNING>("Double signal detected! texture={}, value={}", (void*)this, value);
    }
    m_fenceLastSignaledValue = value;
}
void Texture::SetLastAwaitedValue(uint64_t value) {
    static uint32_t s_waitCount = 0;
    s_waitCount++;
    if (s_waitCount % 500 == 0 || m_fenceLastAwaitedValue == value) {
        Log::print<INTEROP>("Semaphore wait #{}: texture={}, value {} -> {} (last signaled={})",
                            s_waitCount, (void*)this, m_fenceLastAwaitedValue, value, m_fenceLastSignaledValue);
    }
    if (m_fenceLastAwaitedValue == value && value != 0) {
        Log::print<WARNING>("Double wait detected! texture={}, value={}", (void*)this, value);
    }
    m_fenceLastAwaitedValue = value;
}

void Texture::TrackSignaledValue(uint64_t value) { SetLastSignalledValue(value); }
void Texture::TrackWaitedValue(uint64_t value)   { SetLastAwaitedValue(value); }

void Texture::TransitionLayout(VkCommandBuffer cmdList, VkImageLayout newLayout) {
    if (m_composerLayout == newLayout) return;
    VkImageMemoryBarrier2 b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = m_composerLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = m_composerImage;
    b.subresourceRange.aspectMask = VulkanUtils::GetAspectMaskForFormat(m_composerFormat);
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmdList, &di);
    m_composerLayout = newLayout;
}

// ===================================================================== //
// SharedTexture — dual identity.
//
// Linux flow (replaces the Windows D3D12-creates / Vulkan-imports flow):
//  1. Allocate VkImage + VkDeviceMemory on the *Composer* device, with
//     VkExportMemoryAllocateInfo (OPAQUE_FD). Bind. Make a VkImageView.
//  2. vkGetMemoryFdKHR to obtain an int fd for the memory.
//  3. Create a timeline VkSemaphore on the Composer device with
//     VkExportSemaphoreCreateInfo (OPAQUE_FD). vkGetSemaphoreFdKHR for the fd.
//  4. On the *Cemu* device: allocate matching VkImage with
//     VkExternalMemoryImageCreateInfo (OPAQUE_FD), import memory via
//     VkImportMemoryFdInfoKHR, bind. The two VkImage handles alias the same
//     physical pages.
//  5. Create timeline VkSemaphore on Cemu device, import fd via
//     VkImportSemaphoreFdInfoKHR. The two semaphores share state.
// ===================================================================== //

SharedTexture::SharedTexture(uint32_t width, uint32_t height, VkFormat vkFormat, VkFormat composerFormat)
    : Texture(width, height, composerFormat)
    , BaseVulkanTexture(width, height, vkFormat)
{
    VkDevice composer = VRManager::instance().Composer->GetDevice();

    // ---- 1. Composer side: exportable VkImage + memory ----
    VkExternalMemoryImageCreateInfo extImgInfo = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImgInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.pNext = &extImgInfo;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = composerFormat;
    ici.extent = { m_width, m_height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = (VulkanUtils::IsDepthFormat(composerFormat) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    checkVkResult(vkCreateImage(composer, &ici, nullptr, &m_composerImage), "SharedTexture composer image");

    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(composer, m_composerImage, &mr);

    VkExportMemoryAllocateInfo exportInfo = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicated.pNext = &exportInfo;
    dedicated.image = m_composerImage;

    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.pNext = &dedicated;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = VRManager::instance().Composer->FindMemoryType(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVkResult(vkAllocateMemory(composer, &mai, nullptr, &m_composerMemory), "SharedTexture composer memory");
    checkVkResult(vkBindImageMemory(composer, m_composerImage, m_composerMemory, 0), "SharedTexture composer bind");

    // Composer-side image view (for descriptor set / dynamic rendering use).
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = m_composerImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = composerFormat;
    vci.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
                       VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY };
    vci.subresourceRange.aspectMask = VulkanUtils::GetAspectMaskForFormat(composerFormat);
    vci.subresourceRange.levelCount = 1;
    vci.subresourceRange.layerCount = 1;
    checkVkResult(vkCreateImageView(composer, &vci, nullptr, &m_composerImageView), "SharedTexture composer view");

    // ---- 2. Export memory fd ----
    static PFN_vkGetMemoryFdKHR pfn_vkGetMemoryFdKHR = nullptr;
    if (!pfn_vkGetMemoryFdKHR) {
        pfn_vkGetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(composer, "vkGetMemoryFdKHR");
        checkAssert(pfn_vkGetMemoryFdKHR != nullptr, "vkGetMemoryFdKHR unavailable on composer device");
    }
    VkMemoryGetFdInfoKHR memFdGet = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR };
    memFdGet.memory = m_composerMemory;
    memFdGet.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    checkVkResult(pfn_vkGetMemoryFdKHR(composer, &memFdGet, &m_memoryFd), "vkGetMemoryFdKHR");

    // ---- 3. Exportable timeline semaphore ----
    VkSemaphoreTypeCreateInfo timelineCi = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCi.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCi.initialValue = 0;
    VkExportSemaphoreCreateInfo expSemCi = { VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
    expSemCi.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    expSemCi.pNext = &timelineCi;
    VkSemaphoreCreateInfo semCi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semCi.pNext = &expSemCi;
    checkVkResult(vkCreateSemaphore(composer, &semCi, nullptr, &m_timelineSemaphore), "SharedTexture composer semaphore");

    static PFN_vkGetSemaphoreFdKHR pfn_vkGetSemaphoreFdKHR = nullptr;
    if (!pfn_vkGetSemaphoreFdKHR) {
        pfn_vkGetSemaphoreFdKHR = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(composer, "vkGetSemaphoreFdKHR");
        checkAssert(pfn_vkGetSemaphoreFdKHR != nullptr, "vkGetSemaphoreFdKHR unavailable on composer device");
    }
    VkSemaphoreGetFdInfoKHR semFdGet = { VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR };
    semFdGet.semaphore = m_timelineSemaphore;
    semFdGet.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    checkVkResult(pfn_vkGetSemaphoreFdKHR(composer, &semFdGet, &m_semaphoreFd), "vkGetSemaphoreFdKHR");

    // ---- 4. Cemu side: image + import memory ----
    const auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    VkDevice cemu = VRManager::instance().VK->GetDevice();

    VkExternalMemoryImageCreateInfo extImgInfoCemu = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    extImgInfoCemu.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo iciCemu = ici; // same metadata as composer side
    iciCemu.pNext = &extImgInfoCemu;
    iciCemu.format = vkFormat;
    checkVkResult(dispatch->CreateImage(cemu, &iciCemu, nullptr, &m_vkImage), "SharedTexture Cemu image");

    VkMemoryRequirements2 reqs = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    VkImageMemoryRequirementsInfo2 reqInfo = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2 };
    reqInfo.image = m_vkImage;
    dispatch->GetImageMemoryRequirements2(cemu, &reqInfo, &reqs);

    // Query memory-fd properties (which memory types accept this fd).
    // NVIDIA's driver returns VK_ERROR_UNKNOWN (-13) here when the fd was
    // exported from a separate VkDevice (composer's), even on the same
    // physical device — but the subsequent vkAllocateMemory + import still
    // works fine. Fall back to the image's natural memoryTypeBits in that
    // case instead of aborting.
    VkMemoryFdPropertiesKHR fdProps = { VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
    const VkResult fdPropRes = dispatch->GetMemoryFdPropertiesKHR(cemu,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, m_memoryFd, &fdProps);
    if (fdPropRes != VK_SUCCESS) {
        Log::print<WARNING>("vkGetMemoryFdPropertiesKHR returned {} on Cemu device; falling back to image memoryTypeBits=0x{:x}",
                            (int)fdPropRes, reqs.memoryRequirements.memoryTypeBits);
        fdProps.memoryTypeBits = reqs.memoryRequirements.memoryTypeBits;
    }

    VkImportMemoryFdInfoKHR importMemInfo = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR };
    importMemInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    importMemInfo.fd = m_memoryFd;
    VkMemoryDedicatedAllocateInfo dedicatedCemu = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
    dedicatedCemu.pNext = &importMemInfo;
    dedicatedCemu.image = m_vkImage;

    VkMemoryAllocateInfo maiCemu = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    maiCemu.pNext = &dedicatedCemu;
    maiCemu.allocationSize = reqs.memoryRequirements.size;
    maiCemu.memoryTypeIndex = VRManager::instance().VK->FindMemoryType(
        fdProps.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    checkVkResult(dispatch->AllocateMemory(cemu, &maiCemu, nullptr, &m_vkMemory), "SharedTexture Cemu memory import");
    // After AllocateMemory consumes the fd, do not close it manually — the
    // VK_KHR_external_memory_fd spec gives ownership to the implementation.
    m_memoryFd = -1;

    VkBindImageMemoryInfo bii = { VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO };
    bii.image = m_vkImage;
    bii.memory = m_vkMemory;
    checkVkResult(dispatch->BindImageMemory2(cemu, 1, &bii), "SharedTexture Cemu bind");

    // ---- 5. Cemu-side semaphore: create empty, then import fd ----
    VkSemaphoreTypeCreateInfo timelineCiCemu = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    timelineCiCemu.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    timelineCiCemu.initialValue = 0;
    VkSemaphoreCreateInfo semCiCemu = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    semCiCemu.pNext = &timelineCiCemu;
    checkVkResult(dispatch->CreateSemaphore(cemu, &semCiCemu, nullptr, &m_cemuTimelineSemaphore), "SharedTexture Cemu semaphore");

    VkImportSemaphoreFdInfoKHR importSemInfo = { VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR };
    importSemInfo.semaphore = m_cemuTimelineSemaphore;
    importSemInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
    importSemInfo.fd = m_semaphoreFd;
    importSemInfo.flags = 0;
    checkVkResult(dispatch->ImportSemaphoreFdKHR(cemu, &importSemInfo), "vkImportSemaphoreFdKHR");
    // ImportSemaphoreFdKHR with OPAQUE_FD takes ownership of the fd.
    m_semaphoreFd = -1;
}

SharedTexture::~SharedTexture() {
    // Destroy the Cemu-side semaphore handle (the base BaseVulkanTexture dtor
    // handles the composer-side m_timelineSemaphore; Cemu-side is ours).
    if (m_cemuTimelineSemaphore != VK_NULL_HANDLE) {
        if (auto* vk = VRManager::instance().VK.get()) {
            if (const auto* dispatch = vk->GetDeviceDispatch()) {
                dispatch->DestroySemaphore(vk->GetDevice(), m_cemuTimelineSemaphore, nullptr);
            }
        }
        m_cemuTimelineSemaphore = VK_NULL_HANDLE;
    }
    if (m_memoryFd != -1) ::close(m_memoryFd);
    if (m_semaphoreFd != -1) ::close(m_semaphoreFd);
}

void SharedTexture::Init(const VkCommandBuffer& cmdBuffer) {
    // Transition Cemu-side image to GENERAL for interop usage.
    VulkanUtils::TransitionLayout(cmdBuffer, m_vkImage, m_vkCurrLayout, VK_IMAGE_LAYOUT_GENERAL, GetAspectMask());
    m_vkCurrLayout = VK_IMAGE_LAYOUT_GENERAL;
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);

    // Composer-side: transition to GENERAL via an immediate command buffer.
    RND_VkComposer::CommandContext<true> transition(VRManager::instance().Composer.get(),
        [this](RND_VkComposer::CommandContext<true>* context) {
            this->Texture::TransitionLayout(context->GetRecordList(), VK_IMAGE_LAYOUT_GENERAL);
        });
}

void SharedTexture::CopyFromVkImage(VkCommandBuffer cmdBuffer, VkImage srcImage) {
    static uint32_t s_copyCount = 0;
    s_copyCount++;

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    if (srcImage == VK_NULL_HANDLE) {
        Log::print<ERROR>("CopyFromVkImage #{}: srcImage is NULL!", s_copyCount);
        return;
    }
    if (m_vkImage == VK_NULL_HANDLE) {
        Log::print<ERROR>("CopyFromVkImage #{}: dst m_vkImage is NULL!", s_copyCount);
        return;
    }
    if (s_copyCount % 500 == 0) {
        Log::print<INTEROP>("CopyFromVkImage #{}: src={}, dst={}, {}x{}",
                            s_copyCount, (void*)srcImage, (void*)m_vkImage, m_width, m_height);
    }

    VkImageAspectFlags aspectMask = GetAspectMask();
    VkImageCopy copy = {
        .srcSubresource = { aspectMask, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { aspectMask, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { m_width, m_height, 1 }
    };
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
    dispatch->CmdCopyImage(cmdBuffer, srcImage, VK_IMAGE_LAYOUT_GENERAL, m_vkImage, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    VulkanUtils::DebugPipelineBarrier(cmdBuffer);
}
