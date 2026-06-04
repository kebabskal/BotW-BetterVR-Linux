#include "pch.h"

#include "framebuffer.h"
#include "instance.h"
#include "layer.h"
#include "utils/vulkan_utils.h"
#include "utils/debug_draw.h"
#include "utils/render_utils.h"


std::mutex lockImageResolutions;
std::unordered_map<VkImage, std::pair<VkExtent2D, VkFormat>> imageResolutions;

std::mutex s_activeCopyMutex;
std::vector<std::pair<VkCommandBuffer, SharedTexture*>> s_activeCopyOperations;

VkImage s_curr3DColorImage = VK_NULL_HANDLE;
VkImage s_curr3DDepthImage = VK_NULL_HANDLE;

using namespace VRLayer;

VkResult VkDeviceOverrides::CreateImage(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, const VkImageCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkImage* pImage) {
    VkResult res = pDispatch.CreateImage(device, pCreateInfo, pAllocator, pImage);

    // Track every image so the magic-clear hook can look up any candidate's format.
    // Filtering by >=1280x720 previously dropped BotW's depth-stencil image on Linux,
    // breaking 3D depth capture and Is3DComplete().
    lockImageResolutions.lock();
    imageResolutions.try_emplace(*pImage, std::make_pair(VkExtent2D{ pCreateInfo->extent.width, pCreateInfo->extent.height }, pCreateInfo->format));
    lockImageResolutions.unlock();
    return res;
}

void VkDeviceOverrides::DestroyImage(const vkroots::VkDeviceDispatch& pDispatch, VkDevice device, VkImage image, const VkAllocationCallbacks* pAllocator) {
    lockImageResolutions.lock();
    imageResolutions.erase(image);
    if (s_curr3DColorImage == image) {
        s_curr3DColorImage = VK_NULL_HANDLE;
    }
    else if (s_curr3DDepthImage == image) {
        s_curr3DDepthImage = VK_NULL_HANDLE;
    }
    lockImageResolutions.unlock();

    pDispatch.DestroyImage(device, image, pAllocator);
}


void CemuHooks::hook_FixCameraSaveFilesAndInventory(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t originCaller = hCPU->gpr[0];
    uint32_t isEnabling3DFramebufferCapture = hCPU->gpr[3];
    EyeSide side = (EyeSide)hCPU->gpr[4];
    uint32_t frameIdx = hCPU->gpr[5];

    Log::print<PPC>("[{:08X}] hook_FixCameraSaveFilesAndInventory: isEnabling3DFramebufferCapture={:08X}, side={}, frameIdx={}", originCaller, isEnabling3DFramebufferCapture, side, frameIdx);
    VRManager::instance().XR->GetRenderer()->SignalGameCapturing3DFrameBuffer();
}


void VkDeviceOverrides::CmdClearColorImage(const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearColorValue* pColor, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    // check whether the magic values are there, and which order they are in to determine which eye
    OpenXR::EyeSide side = (OpenXR::EyeSide)-1;
    if (pColor->float32[1] >= 0.12 && pColor->float32[1] <= 0.13 && pColor->float32[2] >= 0.97 && pColor->float32[2] <= 0.99) {
        side = OpenXR::EyeSide::LEFT;
    }
    else if (pColor->float32[2] >= 0.12 && pColor->float32[2] <= 0.13 && pColor->float32[1] >= 0.97 && pColor->float32[1] <= 0.99) {
        side = OpenXR::EyeSide::RIGHT;
    }

    // Diagnostic counters — log every 500 calls.
    static std::atomic<uint64_t> s_colorL{0}, s_colorR{0}, s_hud{0};
    {
        static std::atomic<uint64_t> s_totalClears{0};
        static std::atomic<uint64_t> s_magicClears{0};
        const uint64_t total = ++s_totalClears;
        if (side != (OpenXR::EyeSide)-1) ++s_magicClears;
        // captureIdx is r * 32 rounded; 0 = 3D, 2 = 2D
        if (side == OpenXR::EyeSide::LEFT && pColor->float32[0] < 0.05f) ++s_colorL;
        else if (side == OpenXR::EyeSide::RIGHT && pColor->float32[0] < 0.05f) ++s_colorR;
        else if (side != (OpenXR::EyeSide)-1 && pColor->float32[0] > 0.05f) ++s_hud;
        if ((total % 500) == 1) {
            Log::print<INFO>("[BVR-trace] CmdClearColorImage total={} magic={} 3DL={} 3DR={} 2D={} thisColor=({:.3f},{:.3f},{:.3f},{:.3f})",
                total, s_magicClears.load(), s_colorL.load(), s_colorR.load(), s_hud.load(),
                pColor->float32[0], pColor->float32[1], pColor->float32[2], pColor->float32[3]);
        }
    }

    if (!VRManager::instance().VK) {
        auto* dispatch = pDispatch.pDeviceDispatch;
        VRManager::instance().Init(dispatch->pPhysicalDeviceDispatch->pInstanceDispatch->Instance, dispatch->PhysicalDevice, dispatch->Device);
        VRManager::instance().InitSession();
    }

    if (side != (OpenXR::EyeSide)-1) {
        // r value in magical clear value is the capture idx after rounding down
        const long captureIdx = std::lroundf(pColor->float32[0] * 32.0f);
        const long frameIdx = pColor->float32[3] < 0.5f ? 0 : 1;
        checkAssert(captureIdx == 0 || captureIdx == 2, "Invalid capture index!");

        // BetterVR pack quirk: patch_RND_Find2DFrameBuffer.asm uses INVERTED
        // eye convention vs Find3DFrameBuffer (cmpwi r3,1 beq leftEye2DValues
        // — opposite of the 3D patch). So a 2D clear we classify by color as
        // "right" actually targets the LEFT eye's 2D buffer, and vice versa.
        // Flip side for captureIdx==2 (HUD/2D) so the LEFT-side capture
        // branch below (line ~240) actually runs CopyColorToLayer.
        if (captureIdx == 2) {
            side = (side == OpenXR::EyeSide::LEFT) ? OpenXR::EyeSide::RIGHT : OpenXR::EyeSide::LEFT;
        }

        Log::print<RENDERING>("[{}] Clearing color image for {} layer for {} side", frameIdx, captureIdx == 0 ? "3D" : "2D", side == OpenXR::EyeSide::LEFT ? "left" : "right");

        auto* renderer = VRManager::instance().XR->GetRenderer();
        if (!renderer) {
            Log::print<RENDERING>("Renderer is not initialized yet!");
            return pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
        }
        auto& layer3D = renderer->m_layer3D;
        auto& layer2D = renderer->m_layer2D;
        auto& imguiOverlay = renderer->m_imguiOverlay;

        // initialize the textures of both 2D and 3D layer if either is found since they share the same VkImage and resolution
        if (captureIdx == 0 || captureIdx == 2) {
            if (!layer2D) {
                // Look up the cached resolution under the lock and IMMEDIATELY
                // release it before constructing the layers. Layer3D ctor →
                // Swapchain ctor → xrCreateSwapchain re-enters our
                // VkDeviceOverrides::CreateImage hook (because WiVRn allocates
                // its own VkImages on the composer device), and that hook also
                // takes lockImageResolutions. Holding it across construction
                // would be a self-deadlock on the same thread (std::mutex is
                // non-recursive).
                VkExtent2D renderRes;
                VkFormat foundFormat = VK_FORMAT_UNDEFINED;
                bool found = false;
                {
                    std::lock_guard<std::mutex> lk(lockImageResolutions);
                    if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                        renderRes = it->second.first;
                        foundFormat = it->second.second;
                        found = true;
                    }
                }
                if (!found) {
                    checkAssert(false, "Couldn't find image resolution in map!");
                }

                auto viewConfs = VRManager::instance().XR->GetViewConfigurations();
                VkExtent2D swapchainRes = renderRes;
                if (VRManager::instance().XR->m_capabilities.isMetaSimulator) {
                    swapchainRes = VkExtent2D{ viewConfs[0].recommendedImageRectWidth, viewConfs[0].recommendedImageRectHeight };
                }

                renderer->m_gameRenderAspectRatio = (float)renderRes.width / (float)renderRes.height;
                layer3D = std::make_unique<RND_Renderer::Layer3D>(renderRes, swapchainRes);
                layer2D = std::make_unique<RND_Renderer::Layer2D>(renderRes, swapchainRes);
                for (auto& textures : layer3D->GetSharedTextures()) {
                    for (auto& texture : textures) {
                        texture->Init(commandBuffer);
                    }
                }
                for (auto& textures : layer3D->GetDepthSharedTextures()) {
                    for (auto& texture : textures) {
                        texture->Init(commandBuffer);
                    }
                }
                for (auto& texture : layer2D->GetSharedTextures()) {
                    texture->Init(commandBuffer);
                }

                Log::print<INFO>("Found rendering resolution {}x{} @ {} using capture #{}", renderRes.width, renderRes.height, foundFormat, captureIdx);
                imguiOverlay = std::make_unique<RND_Renderer::ImGuiOverlay>(commandBuffer, renderRes, VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                VRManager::instance().Hooks->m_entityDebugger = std::make_unique<EntityDebugger>();
            }
        }

        if (!VRManager::instance().XR->GetRenderer()->IsInitialized()) {
            return;
        }

        checkAssert(layer3D && layer2D, "Couldn't find 3D or 2D layer!");

        // change source image to GENERAL layout
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL);
        VulkanUtils::DebugPipelineBarrier(commandBuffer);

        auto returnToLayout = [&]() {
            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VulkanUtils::DebugPipelineBarrier(commandBuffer);
        };

        RND_Renderer::RenderFrame& frame = renderer->GetFrame(frameIdx);

        auto clearFramebuffer = [&](bool disableAlpha) -> void {
            VkClearColorValue clearColor = disableAlpha ? VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 1.0f } } : VkClearColorValue{ { 0.0f, 0.0f, 0.0f, 0.0f } };
            pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, &clearColor, rangeCount, pRanges);
        };

        // 3D layer - color texture for 3D rendering
        if (captureIdx == 0) {
            // Re-validate format every call instead of latching the first matching image.
            // BotW uses multiple 1280x720 A2B10G10R10 framebuffers (one per ring-buffer
            // slot). The magic clear is fired by BetterVR's PPC patch on the ACTUAL 3D
            // framebuffer, so we trust `image` as long as the format matches.
            bool formatOk = false;
            {
                std::lock_guard<std::mutex> lk(lockImageResolutions);
                if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                    formatOk = (it->second.second == VK_FORMAT_A2B10G10R10_UNORM_PACK32);
                }
            }
            if (formatOk) s_curr3DColorImage = image;

            // don't clear the image if we're in the faux 2D mode
            if (CemuHooks::UseBlackBarsDuringEvents()) {
                returnToLayout();
                return;
            }

            if (!formatOk) {
                returnToLayout();
                return clearFramebuffer(!VRManager::instance().XR->GetRenderer()->IsRendering3D(frameIdx));
            }

            if (renderer->GetFrame(frameIdx).copiedColor[side]) {
                // the color texture has already been copied to the layer
                Log::print<RENDERING>("A 3D color texture is already been copied for the current frame!");

                returnToLayout();
                if (CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
                    return;
                }
                return clearFramebuffer(false);
            }

            // note: This uses vkCmdCopyImage to copy the image to the D3D12-created interop texture. s_activeCopyOperations queues a semaphore for the D3D12 side to wait on.
            SharedTexture* texture = layer3D->CopyColorToLayer(side, commandBuffer, image, frameIdx);
            renderer->On3DColorCopied(side, frameIdx);

            {
                std::lock_guard lk(s_activeCopyMutex);
                s_activeCopyOperations.emplace_back(commandBuffer, texture);
            }

            if (CemuHooks::UseMonoFrameBufferTemporarilyDuringMenusOrPictures()) {
                return;
            }

            // imgui needs only one eye to render Cemu's 2D output, so use right side since it looks better
            if (side == EyeSide::RIGHT) {
                // note: Uses vkCmdCopyImage to copy the (right-eye-only) image to the imgui overlay's texture
                float desktopAspectRatio = layer3D->GetAspectRatio(side);
                const RenderUtils::UvTransform& desktopUvTransform = layer3D->GetPresentUvTransform(side);
                imguiOverlay->Draw3DLayerAsBackground(commandBuffer, image, desktopAspectRatio, desktopUvTransform, frameIdx);
            }

            // clear the image to be transparent to allow for the HUD to be rendered on top of it which results in a transparent HUD layer
            returnToLayout();
            return clearFramebuffer(false);
        }

        // 2D layer - color texture for HUD rendering
        if (captureIdx == 2) {
            bool hudCopied = renderer->GetFrame(frameIdx).copied2D;

            if (side == EyeSide::LEFT) {
                if (hudCopied) {
                    // the 2D texture has already been copied to the layer
                    Log::print<RENDERING>("A 2D texture has already been copied for the current frame!");

                    returnToLayout();
                    return clearFramebuffer(false);
                }
                else {
                    // provide the HUD texture to the imgui overlay we'll use to recomposite Cemu's original flatscreen rendering
                    if (imguiOverlay && !hudCopied) {
                        imguiOverlay->DrawHUDLayerAsBackground(commandBuffer, image, frameIdx);
                        VulkanUtils::DebugPipelineBarrier(commandBuffer);
                    }

                    if (imguiOverlay && !hudCopied) {
                        // render imgui, and then copy the framebuffer to the 2D layer
                        imguiOverlay->Update();
                        imguiOverlay->Render(frameIdx, false, false);
                        imguiOverlay->DrawAndCopyToImage(commandBuffer, image, frameIdx, false);
                        VulkanUtils::DebugPipelineBarrier(commandBuffer);
                    }

                    // copy the HUD texture to D3D12 to be presented
                    // only copy the first attempt at capturing when GX2ClearColor is called with this capture index since the game/Cemu clears the 2D layer twice
                    SharedTexture* texture = layer2D->CopyColorToLayer(commandBuffer, image, frameIdx);
                    renderer->On2DCopied(frameIdx);

                    returnToLayout();
                    {
                        std::lock_guard lk(s_activeCopyMutex);
                        s_activeCopyOperations.emplace_back(commandBuffer, texture);
                    }
                    return;
                }
            }
            if (side == EyeSide::RIGHT) {
                // render the imgui overlay on the right side
                if (imguiOverlay) {
                    // render imgui, and then copy the framebuffer to the 2D layer
                    imguiOverlay->Render(frameIdx, true, true);
                    imguiOverlay->Update();
                    imguiOverlay->DrawAndCopyToImage(commandBuffer, image, frameIdx, true);

                    returnToLayout();
                    return;
                }

                if (hudCopied) {
                    returnToLayout();
                    return clearFramebuffer(false);
                }
            }
        }
        returnToLayout();
        return;
    }
    else {
        return pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout, pColor, rangeCount, pRanges);
    }
}

void VkDeviceOverrides::CmdClearDepthStencilImage(const vkroots::VkCommandBufferDispatch& pDispatch, VkCommandBuffer commandBuffer, VkImage image, VkImageLayout imageLayout, const VkClearDepthStencilValue* pDepthStencil, uint32_t rangeCount, const VkImageSubresourceRange* pRanges) {
    // check for magical clear values
    // check order and whether there's a match with the magical clear value
    OpenXR::EyeSide side = (OpenXR::EyeSide)-1;
    if (pDepthStencil->depth >= 0.011456789 && pDepthStencil->depth <= 0.013456789) { // 0.0123456789
        side = OpenXR::EyeSide::LEFT;
    }
    else if (pDepthStencil->depth >= 0.153987654 && pDepthStencil->depth <= 0.173987654) { // 0.163987654
        side = OpenXR::EyeSide::RIGHT;
    }

    // diag counters
    {
        static std::atomic<uint64_t> s_total{0}, s_dL{0}, s_dR{0};
        const uint64_t total = ++s_total;
        if (side == OpenXR::EyeSide::LEFT) ++s_dL;
        else if (side == OpenXR::EyeSide::RIGHT) ++s_dR;
        if ((total % 500) == 1) {
            Log::print<INFO>("[BVR-trace] CmdClearDepthStencilImage total={} dL={} dR={} thisDepth={:.6f}",
                total, s_dL.load(), s_dR.load(), pDepthStencil->depth);
        }
    }

    if (rangeCount == 1 && side != (OpenXR::EyeSide)-1) {
        // stencil value is the frame counter
        const uint32_t frameCounter = pDepthStencil->stencil;
        checkAssert(frameCounter == 0 || frameCounter == 1, "Invalid frame counter for depth clear!");

        auto& layer3D = VRManager::instance().XR->GetRenderer()->m_layer3D;
        auto& layer2D = VRManager::instance().XR->GetRenderer()->m_layer2D;

        if (!VRManager::instance().XR->GetRenderer()->IsInitialized()) {
            return;
        }

        Log::print<RENDERING>("[{}] Clearing depth image for 3D layer for {} side", frameCounter, side == OpenXR::EyeSide::LEFT ? "left" : "right");

        // change source image to GENERAL layout
        VulkanUtils::TransitionLayout(commandBuffer, image, imageLayout, VK_IMAGE_LAYOUT_GENERAL);
        VulkanUtils::DebugPipelineBarrier(commandBuffer);

        auto returnToLayout = [&]() {
            VulkanUtils::TransitionLayout(commandBuffer, image, VK_IMAGE_LAYOUT_GENERAL, imageLayout);
            VulkanUtils::DebugPipelineBarrier(commandBuffer);
        };

        if (side == OpenXR::EyeSide::LEFT || side == OpenXR::EyeSide::RIGHT) {
            // 3D layer - depth texture. Same reasoning as color: re-validate format every
            // call, don't latch a single image handle. BotW's magic depth clear identifies
            // the actual depth buffer; multiple per-frame depth images exist.
            bool formatOk = false;
            {
                std::lock_guard<std::mutex> lk(lockImageResolutions);
                if (const auto it = imageResolutions.find(image); it != imageResolutions.end()) {
                    formatOk = (it->second.second == VK_FORMAT_D32_SFLOAT);
                }
            }
            if (formatOk) s_curr3DDepthImage = image;

            if (!formatOk) {
                returnToLayout();
                return;
            }

            if (VRManager::instance().XR->GetRenderer()->GetFrame(frameCounter).copiedDepth[side]) {
                // the depth texture has already been copied to the layer
                Log::print<RENDERING>("A depth texture is already bound for the current frame!");
                returnToLayout();
                return;
            }

            // if (layer3D.GetStatus() == Status3D::LEFT_BINDING_DEPTH || layer3D.GetStatus() == Status3D::RIGHT_BINDING_DEPTH) {
            //     // seems to always be the case whenever closing the (inventory) menu
            //     Log::print("A depth texture is already bound for the current frame!");
            //     return;
            // }
            //
            // checkAssert(layer3D.GetStatus() == Status3D::LEFT_BINDING_COLOR || layer3D.GetStatus() == Status3D::RIGHT_BINDING_COLOR, "3D layer is not in the correct state for capturing depth images!");

            SharedTexture* texture = layer3D->CopyDepthToLayer(side, commandBuffer, image, frameCounter);
            VRManager::instance().XR->GetRenderer()->On3DDepthCopied(side, frameCounter);

            {
                std::lock_guard lk(s_activeCopyMutex);
                s_activeCopyOperations.emplace_back(commandBuffer, texture);
            }
            returnToLayout();
            return;
        }
    }
    else {
        return pDispatch.CmdClearDepthStencilImage(commandBuffer, image, imageLayout, pDepthStencil, rangeCount, pRanges);
    }
}

VkResult VkDeviceOverrides::QueueSubmit(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, uint32_t submitCount, const VkSubmitInfo* pSubmits, VkFence fence) {
    VkResult result = VK_SUCCESS;
    
    size_t activeCopyCount;
    {
        std::lock_guard lk(s_activeCopyMutex);
        activeCopyCount = s_activeCopyOperations.size();
    }

    if (activeCopyCount == 0) {
        result = pDispatch.QueueSubmit(queue, submitCount, pSubmits, fence);
    }
    else {
        struct ModifiedSubmitInfo_t {
            VkSubmitInfo submitInfoCopy; // Shadow copy of VkSubmitInfo
            std::vector<VkSemaphore> waitSemaphores;
            std::vector<uint64_t> timelineWaitValues;
            std::vector<VkPipelineStageFlags> waitDstStageMasks;
            std::vector<VkSemaphore> signalSemaphores;
            std::vector<uint64_t> timelineSignalValues;

            VkTimelineSemaphoreSubmitInfo timelineSemaphoreSubmitInfo = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO };
        };

        // insert (possible) pipeline barriers for any active copy operations
        std::vector<ModifiedSubmitInfo_t> modifiedSubmitInfos{ submitCount };
        std::vector<VkSubmitInfo> shadowSubmits{ submitCount };

        std::lock_guard lk(s_activeCopyMutex);

        for (uint32_t i = 0; i < submitCount; i++) {
            const VkSubmitInfo& submitInfo = pSubmits[i];
            ModifiedSubmitInfo_t& modifiedSubmitInfo = modifiedSubmitInfos[i];

            // AMD GPU FIX: Create shadow copy of original VkSubmitInfo
            modifiedSubmitInfo.submitInfoCopy = submitInfo;

            // copy old semaphores into new vectors
            modifiedSubmitInfo.waitSemaphores.assign(submitInfo.pWaitSemaphores, submitInfo.pWaitSemaphores + submitInfo.waitSemaphoreCount);
            modifiedSubmitInfo.waitDstStageMasks.assign(submitInfo.pWaitDstStageMask, submitInfo.pWaitDstStageMask + submitInfo.waitSemaphoreCount);
            modifiedSubmitInfo.timelineWaitValues.resize(submitInfo.waitSemaphoreCount, 0);

            modifiedSubmitInfo.signalSemaphores.assign(submitInfo.pSignalSemaphores, submitInfo.pSignalSemaphores + submitInfo.signalSemaphoreCount);
            modifiedSubmitInfo.timelineSignalValues.resize(submitInfo.signalSemaphoreCount, 0);

            // find timeline semaphore submit info if already present
            const VkTimelineSemaphoreSubmitInfo* existingTimelineInfo = nullptr;

            const VkBaseInStructure* pNextIt = static_cast<const VkBaseInStructure*>(submitInfo.pNext);
            while (pNextIt) {
                if (pNextIt->sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                    existingTimelineInfo = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(pNextIt);
                    break;
                }
                pNextIt = pNextIt->pNext;
            }

            // copy any existing timeline values into new vectors
            if (existingTimelineInfo) {
                for (uint32_t j = 0; j < existingTimelineInfo->waitSemaphoreValueCount; j++) {
                    modifiedSubmitInfo.timelineWaitValues[j] = existingTimelineInfo->pWaitSemaphoreValues[j];
                }
                for (uint32_t j = 0; j < existingTimelineInfo->signalSemaphoreValueCount; j++) {
                    modifiedSubmitInfo.timelineSignalValues[j] = existingTimelineInfo->pSignalSemaphoreValues[j];
                }
            }

            // Insert timeline semaphores for active copy operations
            for (uint32_t j = 0; j < submitInfo.commandBufferCount; j++) {
                for (auto it = s_activeCopyOperations.begin(); it != s_activeCopyOperations.end();) {
                    if (submitInfo.pCommandBuffers[j] == it->first) {
                        // Wait for D3D12/XR to finish with the previous shared texture render
                        uint64_t waitValue = it->second->GetVulkanWaitValue();
                        modifiedSubmitInfo.waitSemaphores.emplace_back(it->second->GetSemaphoreForWait(waitValue));
                        modifiedSubmitInfo.waitDstStageMasks.emplace_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
                        modifiedSubmitInfo.timelineWaitValues.emplace_back(waitValue);

                        // Signal to D3D12/XR rendering that the shared texture can be rendered to VR headset
                        uint64_t signalValue = it->second->GetVulkanSignalValue();
                        modifiedSubmitInfo.signalSemaphores.emplace_back(it->second->GetSemaphoreForSignal(signalValue));
                        modifiedSubmitInfo.timelineSignalValues.emplace_back(signalValue);
                        it = s_activeCopyOperations.erase(it);
                    }
                    else {
                        ++it;
                    }
                }
            }

            // Update timeline semaphore submit info
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.waitSemaphoreValueCount = (uint32_t)modifiedSubmitInfo.timelineWaitValues.size();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pWaitSemaphoreValues = modifiedSubmitInfo.timelineWaitValues.data();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.signalSemaphoreValueCount = (uint32_t)modifiedSubmitInfo.timelineSignalValues.size();
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pSignalSemaphoreValues = modifiedSubmitInfo.timelineSignalValues.data();

            // AMD GPU FIX: Preserve existing pNext chain - prepend our timeline struct
            modifiedSubmitInfo.timelineSemaphoreSubmitInfo.pNext = submitInfo.pNext;

            modifiedSubmitInfo.submitInfoCopy.pNext = &modifiedSubmitInfo.timelineSemaphoreSubmitInfo;
            modifiedSubmitInfo.submitInfoCopy.waitSemaphoreCount = (uint32_t)modifiedSubmitInfo.waitSemaphores.size();
            modifiedSubmitInfo.submitInfoCopy.pWaitSemaphores = modifiedSubmitInfo.waitSemaphores.data();
            modifiedSubmitInfo.submitInfoCopy.pWaitDstStageMask = modifiedSubmitInfo.waitDstStageMasks.data();
            modifiedSubmitInfo.submitInfoCopy.signalSemaphoreCount = (uint32_t)modifiedSubmitInfo.signalSemaphores.size();
            modifiedSubmitInfo.submitInfoCopy.pSignalSemaphores = modifiedSubmitInfo.signalSemaphores.data();

            shadowSubmits[i] = modifiedSubmitInfo.submitInfoCopy;
        }
        result = pDispatch.QueueSubmit(queue, submitCount, shadowSubmits.data(), fence);
    }

    if (result != VK_SUCCESS) {
        Log::print<ERROR>("QueueSubmit failed with error {}", result);
    }

    return result;
}

VkResult VkDeviceOverrides::QueuePresentKHR(const vkroots::VkQueueDispatch& pDispatch, VkQueue queue, const VkPresentInfoKHR* pPresentInfo) {
    VRManager::instance().XR->ProcessEvents();

    auto* renderer = VRManager::instance().XR->GetRenderer();
    {
        static std::atomic<uint64_t> s_presents{0};
        static std::atomic<uint64_t> s_framesRun{0};
        const uint64_t n = ++s_presents;
        const bool layersReady = renderer && renderer->m_layer3D && renderer->m_layer2D && renderer->m_imguiOverlay;
        if (layersReady) ++s_framesRun;
        if ((n % 120) == 1) {
            Log::print<INFO>("[BVR-trace] QueuePresentKHR n={} renderer={} layersReady={} framesRun={}",
                n, renderer ? "set" : "null", layersReady ? "y" : "n", s_framesRun.load());
        }
    }
    if (renderer && renderer->m_layer3D && renderer->m_layer2D && renderer->m_imguiOverlay) {
        if (renderer->IsInitialized()) {
            renderer->EndFrame();
        }
        renderer->StartFrame();
    }

    return pDispatch.QueuePresentKHR(queue, pPresentInfo);
}
