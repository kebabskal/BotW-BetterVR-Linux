#pragma once

// VR-side Vulkan compositor backend.
//
// This file is the Linux-port equivalent of upstream's src/rendering/d3d12.h.
// The architecture is preserved 1:1 — every method, RAII helper, and pipeline
// template that existed on the D3D12 side has a Vulkan counterpart here. The
// only changes are the types and the underlying API.
//
// Key mappings (kept consistent throughout the port):
//   ID3D12Device                  → VkDevice
//   ID3D12CommandQueue            → VkQueue
//   ID3D12Fence + HANDLE event    → VkSemaphore (timeline) + vkWaitSemaphores
//   ID3D12GraphicsCommandList     → VkCommandBuffer
//   ID3D12CommandAllocator        → VkCommandPool
//   ID3D12RootSignature           → VkPipelineLayout + VkDescriptorSetLayout(s)
//   ID3D12PipelineState           → VkPipeline (graphics)
//   ID3D12DescriptorHeap (CBV_SRV_UAV) → VkDescriptorPool + VkDescriptorSet[]
//   D3D12_CPU_DESCRIPTOR_HANDLE   → VkImageView + descriptor binding write
//   ID3D12Resource (buffer)       → VkBuffer + VkDeviceMemory
//   DXGI_FORMAT                   → VkFormat
//
// Cross-API sharing on Linux: D3D12 used NT-shared HANDLEs for VkImage memory
// and VkFence/VkSemaphore. On Linux the equivalent is opaque-fd:
//   VK_KHR_external_memory_fd      (image memory)
//   VK_KHR_external_semaphore_fd   (sync primitives)
// SharedTexture (see texture.h) handles the import/export across the two
// Vulkan instances (Cemu's and the composer's).

#include "openxr.h"
#include "texture.h"
#include "utils/render_utils.h"
#include "utils/debug_draw.h"

class RND_Renderer;

class RND_VkComposer {
    friend class RND_Renderer;

public:
    RND_VkComposer();
    ~RND_VkComposer();

    VkInstance       GetInstance()        const { return m_instance; }
    VkPhysicalDevice GetPhysicalDevice()  const { return m_physicalDevice; }
    VkDevice         GetDevice()          const { return m_device; }
    VkQueue          GetCommandQueue()    const { return m_queue; }
    uint32_t         GetQueueFamilyIndex() const { return m_queueFamilyIndex; }

    // Frame-context lifecycle (matches D3D12's StartFrame/EndFrame contract:
    // rotates through three frame contexts, waiting for the prior occupant of
    // the rotated-in slot to finish on the GPU).
    void StartFrame();
    void EndFrame();

    // ---- PresentPipeline ----
    // Renders an attachment texture (color, optional depth + fade-sample) into
    // an OpenXR swapchain image. On D3D12 this was descriptor-table+RTV+DSV;
    // on Vulkan it's a descriptor set + dynamic rendering. Templated on `depth`
    // to switch between the simple 1-input present and the 3-input
    // present-with-depth-and-fade variant.
    template <bool depth>
    class PresentPipeline {
        friend class Texture;

    public:
        explicit PresentPipeline(RND_Renderer* pRenderer);
        ~PresentPipeline();

        // Bind an input texture (SRV in D3D12 = sampled image in Vulkan).
        // `srcImageView`/`srcFormat` may be reused across frames; the binding
        // is written into the descriptor set on each call.
        void BindAttachment(uint32_t attachmentIdx, VkImageView srcImageView, VkFormat srcFormat = VK_FORMAT_UNDEFINED);
        // Bind the target swapchain image as a render target.
        void BindTarget(uint32_t targetIdx, VkImageView dstImageView, VkFormat dstFormat = VK_FORMAT_UNDEFINED);
        // Bind a depth render target (only used in the depth=true variant).
        void BindDepthTarget(VkImageView dstImageView, VkFormat dstFormat);
        // Per-frame UV/screen settings (also creates the upload buffer on first call).
        void BindSettings(float screenWidth, float screenHeight, const RenderUtils::UvTransform& uvTransform = {});
        void SetUvTransform(const RenderUtils::UvTransform& uvTransform) { m_uvTransform = uvTransform; }
        // Issue the draw. Takes either an extent (preferred) or just an image
        // view of the target — the destination image itself isn't read from.
        void Render(VkCommandBuffer commandBuffer, VkImageView targetView);
        void Render(VkCommandBuffer commandBuffer, VkImageView targetView, VkExtent2D explicitExtent);

    private:
        void UpdateSettingsBuffer(VkExtent2D swapchainExtent);
        void RecreatePipeline();
        void EnsureDescriptorSet();
        void WriteAttachmentDescriptors();

        RND_Renderer* m_renderer = nullptr;
        float m_renderWidth = 0.0f;
        float m_renderHeight = 0.0f;
        RenderUtils::UvTransform m_uvTransform = {};

        // Cached SPIR-V (compiled once at PresentPipeline construction).
        std::vector<uint32_t> m_vertexSpirv;
        std::vector<uint32_t> m_pixelSpirv;
        VkShaderModule m_vertexShader = VK_NULL_HANDLE;
        VkShaderModule m_pixelShader = VK_NULL_HANDLE;

        // Vertex data — D3D12 had a screenIndices buffer (4 verts → 2 tris).
        // Vulkan equivalent: small index buffer. Vertices are generated from
        // gl_VertexIndex in the shader, matching the HLSL SV_VertexID trick.
        VkBuffer        m_screenIndicesBuffer  = VK_NULL_HANDLE;
        VkDeviceMemory  m_screenIndicesMemory  = VK_NULL_HANDLE;

        // CBV b1 → uniform buffer for present settings (mapped, persistent).
        VkBuffer        m_settingsBuffer       = VK_NULL_HANDLE;
        VkDeviceMemory  m_settingsMemory       = VK_NULL_HANDLE;
        void*           m_settingsMapped       = nullptr;

        // Pipeline layout + sampler are constant for the lifetime of the
        // PresentPipeline; pipeline itself can be re-built when the target
        // format changes.
        VkDescriptorSetLayout m_setLayout      = VK_NULL_HANDLE;
        VkPipelineLayout      m_pipelineLayout = VK_NULL_HANDLE;
        VkSampler             m_sampler        = VK_NULL_HANDLE;
        VkPipeline            m_pipeline       = VK_NULL_HANDLE;
        VkDescriptorPool      m_descriptorPool = VK_NULL_HANDLE;
        VkDescriptorSet       m_descriptorSet  = VK_NULL_HANDLE;

        // Per-attachment bindings cached so we can re-write the descriptor
        // set every frame without re-allocating.
        struct AttachmentBinding {
            VkImageView view = VK_NULL_HANDLE;
            VkFormat    format = VK_FORMAT_UNDEFINED;
        };
        std::array<AttachmentBinding, depth ? 3 : 1> m_attachmentBindings = {};
        std::array<AttachmentBinding, 1>            m_targetBindings = {};
        std::array<AttachmentBinding, depth ? 1 : 0> m_depthTargetBindings = {};
        std::array<VkFormat, 2> m_targetFormats = { VK_FORMAT_UNDEFINED, VK_FORMAT_D32_SFLOAT };
    };

    // ---- CommandContext ----
    // RAII helper that mirrors D3D12 exactly:
    //   - Acquire a command buffer (per-frame pool or immediate pool)
    //   - Run the recorder callback
    //   - End + Submit, with optional waitFor/signal on per-texture timeline
    //     semaphores
    //   - If blockTillExecuted, vkWaitSemaphores on the queue completion
    //     semaphore before returning
    template <bool blockTillExecuted>
    class CommandContext {
    public:
        template <typename F>
        CommandContext(RND_VkComposer* composer, F&& recordCallback)
            : m_composer(composer)
        {
            checkAssert(m_composer != nullptr, "Vulkan command context is missing its renderer backend!");
            m_waitFor.reserve(4);
            m_signalTo.reserve(4);

            if constexpr (blockTillExecuted) {
                m_cmdBuffer = m_composer->AcquireImmediateCommandBuffer();
            } else {
                m_cmdBuffer = m_composer->AcquireFrameCommandBuffer();
            }

            recordCallback(this);
        }

        ~CommandContext();

        VkCommandBuffer GetRecordList() { return m_cmdBuffer; }
        void WaitFor(Texture* texture, uint64_t value) { m_waitFor.push_back({ texture, value }); }
        void Signal(Texture* texture, uint64_t value) { m_signalTo.push_back({ texture, value }); }

    private:
        RND_VkComposer* m_composer = nullptr;
        VkCommandBuffer m_cmdBuffer = VK_NULL_HANDLE;
        std::vector<std::pair<Texture*, uint64_t>> m_waitFor;
        std::vector<std::pair<Texture*, uint64_t>> m_signalTo;
    };

    // ---- DebugDrawPipeline ----
    // 1:1 port of the D3D12 version. Renders debug triangles / lines with an
    // x-ray variant that disables depth test. Used by Layer3D::Render.
    class DebugDrawPipeline {
    public:
        DebugDrawPipeline();
        ~DebugDrawPipeline();

        void Render(OpenXR::EyeSide side,
                    VkCommandBuffer commandBuffer,
                    VkImageView sceneDepthView,
                    VkImageView colorTargetView, VkFormat colorFormat, VkExtent2D colorExtent,
                    VkImageView depthTargetView, VkFormat depthFormat,
                    const DebugDrawRenderData& renderData,
                    const glm::mat4& viewProjection);

    private:
        void RecreatePipeline();
        void EnsureVertexBuffer(uint32_t requiredBytes);
        void UpdateSceneSettings(OpenXR::EyeSide side, uint32_t settingsIndex,
                                 VkExtent2D colorExtent,
                                 const glm::mat4& viewProjection,
                                 float xrayAlphaScale);
        void RenderVertices(VkCommandBuffer commandBuffer, VkPipeline pipeline,
                            uint32_t vertexOffset, uint32_t vertexCount);
        VkDescriptorSet AcquireSceneDescriptorSet(OpenXR::EyeSide side, VkImageView sceneDepthView);

        std::vector<uint32_t> m_vertexSpirv;
        std::vector<uint32_t> m_pixelSpirv;
        VkShaderModule m_vertexShader = VK_NULL_HANDLE;
        VkShaderModule m_pixelShader = VK_NULL_HANDLE;

        VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
        VkPipelineLayout m_pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
        VkSampler m_sampler = VK_NULL_HANDLE;

        VkPipeline m_trianglePipeline = VK_NULL_HANDLE;
        VkPipeline m_linePipeline = VK_NULL_HANDLE;
        VkPipeline m_xrayTrianglePipeline = VK_NULL_HANDLE;
        VkPipeline m_xrayLinePipeline = VK_NULL_HANDLE;

        // Per-eye, per-settings-slot uniform buffers (matches D3D12 layout).
        struct SettingsBuffer {
            VkBuffer buffer = VK_NULL_HANDLE;
            VkDeviceMemory memory = VK_NULL_HANDLE;
            void* mapped = nullptr;
        };
        std::array<std::array<SettingsBuffer, 2>, 2> m_sceneSettingsBuffers = {};

        // Per-eye descriptor sets (one per side, two scene-depth bindings).
        std::array<VkDescriptorSet, 2> m_sceneDescriptorSets = { VK_NULL_HANDLE, VK_NULL_HANDLE };

        // Vertex buffer (host-mapped). Grows on demand.
        VkBuffer m_vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory m_vertexBufferMemory = VK_NULL_HANDLE;
        void* m_vertexBufferMapped = nullptr;
        uint32_t m_vertexBufferCapacity = 0;

        std::array<VkFormat, 2> m_targetFormats = { VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED };
    };

    // ---- Memory + sync helpers (used by Texture etc.) ----
    uint32_t FindMemoryType(uint32_t memoryTypeBitsRequirement, VkMemoryPropertyFlags requirementsMask) const;
    VkSemaphore CreateTimelineSemaphore(uint64_t initialValue = 0) const;

private:
    static constexpr uint32_t kFrameContextCount = 3;
    static constexpr uint32_t kFrameCommandBufferCount = 4;

    struct FrameContext {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        std::array<VkCommandBuffer, kFrameCommandBufferCount> commandBuffers = {};
        uint32_t nextCommandBufferIndex = 0;
        uint64_t completionFenceValue = 0;
    };

    struct ImmediateContext {
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    };

    FrameContext& GetCurrentFrameContext();
    VkCommandBuffer AcquireFrameCommandBuffer();
    VkCommandBuffer AcquireImmediateCommandBuffer();
    void ExecuteCommandBuffer(VkCommandBuffer commandBuffer,
                              std::span<const std::pair<Texture*, uint64_t>> waitFor,
                              std::span<const std::pair<Texture*, uint64_t>> signalTo);
    uint64_t SignalQueueFence();
    void WaitForQueueFence(uint64_t fenceValue);

    void CreateInstanceAndDevice();

    VkInstance m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamilyIndex = 0;
    VkPhysicalDeviceMemoryProperties m_memoryProperties = {};

    // Per-frame command pools + reusable command buffers.
    std::array<FrameContext, kFrameContextCount> m_frameContexts = {};
    ImmediateContext m_immediateContext = {};
    uint32_t m_currentFrameContextIndex = kFrameContextCount - 1;

    // Queue completion timeline semaphore (replaces ID3D12Fence + event).
    VkSemaphore m_queueTimeline = VK_NULL_HANDLE;
    uint64_t m_nextFenceValue = 1;
};

// Backwards-compat alias so the rest of the codebase can keep using the
// short name `RND_D3D12` where it'd be too invasive to rename every callsite.
// (We DO rename in the cleaned-up files; this exists only as a safety net.)
// Comment out to enforce explicit renames.
// using RND_D3D12 = RND_VkComposer;
