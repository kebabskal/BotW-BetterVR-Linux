#pragma once

#include "pch.h"
#include "vk_composer.h"
#include "openxr.h"
#include "swapchain.h"
#include "texture.h"
#include "utils/debug_draw.h"

class SharedTexture;

class RND_Renderer {
public:
    explicit RND_Renderer(XrSession xrSession);
    ~RND_Renderer();

    struct RenderFrame {
        std::optional<std::array<XrView, 2>> views;
        std::atomic_bool copiedColor[2] = { false, false };
        std::atomic_bool copiedDepth[2] = { false, false };
        std::atomic_bool copied2D = false;
        std::atomic_bool presented3D = false;
        std::atomic_uint8_t cameraIsCapturing3DFramebuffer = 0;

        std::unique_ptr<VulkanTexture> mainFramebuffer;
        std::unique_ptr<VulkanTexture> hudFramebuffer;
        std::unique_ptr<VulkanTexture> hudWithoutAlphaFramebuffer;
        std::unique_ptr<VulkanFramebuffer> imguiFramebuffer;
        VkDescriptorSet mainFramebufferDS = VK_NULL_HANDLE;
        VkDescriptorSet hudFramebufferDS = VK_NULL_HANDLE;
        VkDescriptorSet hudWithoutAlphaFramebufferDS = VK_NULL_HANDLE;
        float mainFramebufferAspectRatio = 1.0f;
        RenderUtils::UvTransform mainFramebufferUvTransform = {};

        bool ranMotionAnalysis[2] = { false, false };

        bool Is3DComplete() const { return copiedColor[0] && copiedColor[1] && copiedDepth[0] && copiedDepth[1]; }
        bool Is2DComplete() const { return copied2D; }

        void Reset() {
            views = std::nullopt;
            copiedColor[0] = false;
            copiedColor[1] = false;
            copiedDepth[0] = false;
            copiedDepth[1] = false;
            copied2D = false;
            if (cameraIsCapturing3DFramebuffer > 0)
                --cameraIsCapturing3DFramebuffer;

            ranMotionAnalysis[0] = false;
            ranMotionAnalysis[1] = false;
        }
    };

    void StartFrame();
    void EndFrame();
    std::optional<std::array<XrView, 2>> UpdateViews(XrTime predictedDisplayTime);
    
    std::optional<std::array<XrView, 2>> GetPoses(long frameIdx = -1) const { 
        if (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) return m_renderFrames[frameIdx].views;
        return m_currViews; 
    }
    
    std::optional<XrFovf> GetFOV(OpenXR::EyeSide side, long frameIdx = -1) const { 
        const auto& views = (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) ? m_renderFrames[frameIdx].views : m_currViews;
        return views.transform([side](auto& views) { return views[side].fov; }); 
    }
    
    std::optional<XrPosef> GetPose(OpenXR::EyeSide side, long frameIdx = -1) const { 
        const auto& views = (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) ? m_renderFrames[frameIdx].views : m_currViews;
        return views.transform([side](auto& views) { return views[side].pose; }); 
    }
    
    std::optional<glm::fmat4> GetPoseAsMatrix(OpenXR::EyeSide side, long frameIdx = -1) const {
        const auto& views = (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) ? m_renderFrames[frameIdx].views : m_currViews;
        return views.transform([side](auto& views) {
            const XrPosef& pose = views[side].pose;
            return ToMat4(ToGLM(pose.position), ToGLM(pose.orientation));
        });
    };
    
    std::optional<glm::fmat4> GetMiddlePose(long frameIdx = -1) const {
        const auto& views = (frameIdx != -1 && m_renderFrames[frameIdx].views.has_value()) ? m_renderFrames[frameIdx].views : m_currViews;
        if (!views.has_value()) return std::nullopt;
        const XrPosef& leftPose = views->at(OpenXR::EyeSide::LEFT).pose;
        const XrPosef& rightPose = views->at(OpenXR::EyeSide::RIGHT).pose;
        glm::fvec3 middlePos = (ToGLM(leftPose.position) + ToGLM(rightPose.position)) * 0.5f;
        glm::quat middleOri = glm::slerp(ToGLM(leftPose.orientation), ToGLM(rightPose.orientation), 0.5f);

        return ToMat4(middlePos, middleOri);
    };

    double GetLastFrameWorkTimeMs() const { return m_lastFrameWorkTimeMs; }
    double GetLastWaitTimeMs() const { return m_lastWaitTimeMs; }
    double GetLastFrameTimeMs() const { return m_lastFrameTimeMs; }
    double GetPredictedDisplayPeriodMs() const { return m_predictedDisplayPeriodMs; }
    double GetLastOverheadMs() const { return m_lastOverheadMs; }

    void On3DColorCopied(OpenXR::EyeSide side, long frameIdx) {
        m_renderFrames[frameIdx].copiedColor[side] = true;
        if (!m_renderFrames[frameIdx].views.has_value()) m_renderFrames[frameIdx].views = m_currViews;
        DebugDraw::instance().SnapshotEyeState(side, frameIdx);
    }

    void On3DDepthCopied(OpenXR::EyeSide side, long frameIdx) {
        m_renderFrames[frameIdx].copiedDepth[side] = true;
        if (!m_renderFrames[frameIdx].views.has_value()) m_renderFrames[frameIdx].views = m_currViews;
    }

    void On2DCopied(long frameIdx) {
        m_renderFrames[frameIdx].copied2D = true;
    }

    RenderFrame& GetFrame(long frameIdx) { return m_renderFrames[frameIdx]; }
    const RenderFrame& GetFrame(long frameIdx) const { return m_renderFrames[frameIdx]; }

    class Layer3D {
    public:
        explicit Layer3D(VkExtent2D inputRes, VkExtent2D outputRes);
        ~Layer3D();

        SharedTexture* CopyColorToLayer(OpenXR::EyeSide side, VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
        SharedTexture* CopyDepthToLayer(OpenXR::EyeSide side, VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
        void PrepareRendering(OpenXR::EyeSide side);
        void StartRendering();
        void Render(OpenXR::EyeSide side, long frameIdx, SharedTexture* fadeTexture, const DebugDrawRenderData& debugDrawData);
        const std::array<XrCompositionLayerProjectionView, 2>& FinishRendering(long frameIdx);

        float GetAspectRatio(OpenXR::EyeSide side) const { return m_recommendedAspectRatios[side]; }
        const RenderUtils::UvTransform& GetPresentUvTransform(OpenXR::EyeSide side) const { return m_presentUvTransforms[side]; }
        long GetCurrentFrameIdx() const { return m_currentFrameIdx; }
        auto& GetSharedTextures() { return m_textures; }
        auto& GetDepthSharedTextures() { return m_depthTextures; }

    private:
        std::array<std::unique_ptr<Swapchain<VK_FORMAT_R8G8B8A8_SRGB>>, 2> m_swapchains;
        std::array<std::unique_ptr<Swapchain<VK_FORMAT_D32_SFLOAT>>, 2> m_depthSwapchains;
        std::array<std::unique_ptr<RND_VkComposer::PresentPipeline<true>>, 2> m_presentPipelines;
        std::array<std::array<std::unique_ptr<SharedTexture>, 2>, 2> m_textures;
        std::array<std::array<std::unique_ptr<SharedTexture>, 2>, 2> m_depthTextures;
        std::array<float, 2> m_recommendedAspectRatios = { 1.0f, 1.0f };
        std::array<RenderUtils::UvTransform, 2> m_presentUvTransforms = {};
        std::unique_ptr<RND_VkComposer::DebugDrawPipeline> m_debugDrawPipeline;

        std::array<XrCompositionLayerProjectionView, 2> m_projectionViews = {};
        std::array<XrCompositionLayerDepthInfoKHR, 2> m_projectionViewsDepthInfo = {};

        long m_currentFrameIdx = 0;
    };

    class Layer2D {
    public:
        explicit Layer2D(VkExtent2D inputRes, VkExtent2D outputRes);
        ~Layer2D();

        SharedTexture* CopyColorToLayer(VkCommandBuffer copyCmdBuffer, VkImage image, long frameIdx);
        // AMD GPU FIX: With incrementing values, Vulkan signals odd values (1,3,5...), D3D12 signals even values (2,4,6...)
        // Texture is ready for D3D12 when Vulkan has signaled (odd value > 0)
        bool IsTextureReady(long frameIdx) const {
            uint64_t lastSignal = m_textures[frameIdx]->GetLastSignalledValue();
            return lastSignal > 0 && (lastSignal % 2 == 1);
        };
        void StartRendering() const;
        void Render(long frameIdx);
        std::vector<XrCompositionLayerQuad> FinishRendering(XrTime predictedDisplayTime, long frameIdx);
        long GetCurrentFrameIdx() const { return m_currentFrameIdx; }
        auto& GetSharedTextures() { return m_textures; }

        static void SetBowAimingActive(bool isActive) { s_isBowAimingActive = isActive; }
        static bool IsBowAimingActive() { return s_isBowAimingActive; }

    private:
        static std::atomic_bool s_isBowAimingActive;
        std::unique_ptr<Swapchain<VK_FORMAT_R8G8B8A8_SRGB>> m_swapchain;
        std::unique_ptr<RND_VkComposer::PresentPipeline<false>> m_presentPipeline;
        std::array<std::unique_ptr<SharedTexture>, 2> m_textures;

        glm::quat m_currentOrientation = glm::identity<glm::fquat>();

        long m_currentFrameIdx = 0;
    };

    class ImGuiOverlay {
    public:
        explicit ImGuiOverlay(VkCommandBuffer cb,VkExtent2D fbRes, VkFormat framebufferFormat);
        ~ImGuiOverlay();

        bool ShouldBlockGameInput() { return ImGui::GetIO().WantCaptureKeyboard; }

        void Update();
        static void Draw3DLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, float aspectRatio, const RenderUtils::UvTransform& uvTransform, long frameIdx);
        static void DrawHUDLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, long frameIdx);
        void Render(long frameIdx, bool renderBackground, bool isDesktopView);
        void DrawAndCopyToImage(VkCommandBuffer cb, VkImage destImage, long frameIdx, bool isDesktopView);
        void DrawHelpMenu();
        void ProcessInputs(OpenXR::InputState& inputs, const VPADStatus& vpadStatus);
        int GetHelpImagePagesCount() const { return m_helpImages.size(); };

    private:
        void DrawSettingsTab(const ImVec2& windowWidth, bool* changed);
        void DrawDebugTab(bool* changed);
        void DrawHelpGuideTab();
        void DrawFPSOverlayTab(const ImVec2& windowWidth, bool* changed);
        void DrawCreditsTab();
        void DrawCustomAttackSensitivityTab(const ImVec2& windowWidth, bool* changed);

        VkDescriptorPool m_descriptorPool;
        VkRenderPass m_renderPass;
        struct HelpImage {
            const char* m_title = nullptr;
            VulkanTexture* m_image;
            VkDescriptorSet m_imageDS = VK_NULL_HANDLE;
        };
        std::vector<HelpImage> m_helpImages;
        uint32_t m_currentHelpImage = 0;

        VkSampler m_sampler = VK_NULL_HANDLE;
        VkExtent2D m_outputRes = {};
    };

    std::unique_ptr<Layer3D> m_layer3D;
    std::unique_ptr<Layer2D> m_layer2D;
    std::unique_ptr<ImGuiOverlay> m_imguiOverlay;
    float m_gameRenderAspectRatio = 16.0f / 9.0f;

    bool IsRendering3D(long frameIdx) {
        return m_renderFrames[frameIdx].presented3D;
    }
    bool IsRendering2D() {
        return m_presented2DLastFrame;
    }
    bool IsInitialized() {
        return m_isInitialized;
    }
    bool IsGameCapturing3DFrameBuffer() const {
        return m_cameraIsCapturing3DFrameBuffer > 0;
    }
    void SignalGameCapturing3DFrameBuffer() {
        m_cameraIsCapturing3DFrameBuffer = 1;
    }

    void SetCustomFadeAmount(float amount) {
        m_customFadeAmount.store(glm::clamp(amount, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    void SetCustomFadeColor(const glm::fvec3& color) {
        m_customFadeColorR.store(glm::clamp(color.x, 0.0f, 1.0f), std::memory_order_relaxed);
        m_customFadeColorG.store(glm::clamp(color.y, 0.0f, 1.0f), std::memory_order_relaxed);
        m_customFadeColorB.store(glm::clamp(color.z, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    struct CustomFade {
        float amount = 0.0f;
        glm::fvec3 color = glm::fvec3(0.0f);
    };

    CustomFade GetCustomFade() const {
        return {
            .amount = m_customFadeAmount.load(std::memory_order_relaxed),
            .color = {
                m_customFadeColorR.load(std::memory_order_relaxed),
                m_customFadeColorG.load(std::memory_order_relaxed),
                m_customFadeColorB.load(std::memory_order_relaxed),
            },
        };
    }

    bool IsFadeActive() const { return m_isFadeActive.load(std::memory_order_relaxed); }

protected:
    XrSession m_session;
    XrFrameState m_frameState = { XR_TYPE_FRAME_STATE };
    std::optional<std::array<XrView, 2>> m_currViews;
    std::array<RenderFrame, 2> m_renderFrames;

    std::atomic_bool m_isInitialized = false;
    std::atomic_bool m_presented2DLastFrame = false;
    std::atomic_uint8_t m_cameraIsCapturing3DFrameBuffer = 0;
    std::atomic_bool m_isFadeActive = false;
    std::atomic<float> m_customFadeAmount = 0.0f;
    std::atomic<float> m_customFadeColorR = 0.0f;
    std::atomic<float> m_customFadeColorG = 0.0f;
    std::atomic<float> m_customFadeColorB = 0.0f;

    // Full-frame timing derived from OpenXR timestamps (XrTime is in nanoseconds)
    XrTime m_lastPredictedDisplayTime = 0;

    std::chrono::high_resolution_clock::time_point m_frameStartTime;

    double m_lastFrameWorkTimeMs = 0.0;
    double m_lastWaitTimeMs = 0.0;

    // Derived from OpenXR timestamps
    double m_lastFrameTimeMs = 0.0;
    double m_predictedDisplayPeriodMs = 0.0;
    double m_lastOverheadMs = 0.0;
};
