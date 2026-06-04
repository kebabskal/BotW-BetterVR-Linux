#include "pch.h"

#include "vulkan.h"
#include "hooking/cemu_hooks.h"
#include "hooking/imgui_menus.h"
#include "instance.h"
#include "utils/vulkan_utils.h"
#include "utils/mod_settings.h"

#undef __cpuid
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

// include font and assets
#include "imgui_assets.h"
#include "font_kenney.h"
#include "font_roboto.h"


RND_Renderer::ImGuiOverlay::ImGuiOverlay(VkCommandBuffer cb, VkExtent2D fbRes, VkFormat fbFormat): m_outputRes(fbRes) {
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = "BetterVR_settings.ini";
    InitSettings();
    ImGui::LoadIniSettingsFromDisk("BetterVR_settings.ini");
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;

    ImFontConfig fontCfg{};
    fontCfg.OversampleH = 8;
    fontCfg.OversampleV = 8;
    fontCfg.FontDataOwnedByAtlas = false;
    ImFont* textFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(roboto_compressed_data, roboto_compressed_size, 16.0f, &fontCfg);

    ImFontConfig iconCfg{};
    iconCfg.MergeMode = true;
    iconCfg.PixelSnapH = true;
    iconCfg.GlyphMinAdvanceX = 16.0f;
    iconCfg.GlyphOffset = ImVec2(0.0f, 2.0f);
    iconCfg.OversampleH = 8;
    iconCfg.OversampleV = 8;
    iconCfg.FontDataOwnedByAtlas = false;
    static const ImWchar icon_ranges[] = { ICON_MIN_KI, ICON_MAX_16_KI, 0 };
    iconCfg.GlyphRanges = icon_ranges;
    ImFont* iconFont = ImGui::GetIO().Fonts->AddFontFromMemoryCompressedTTF(kenney_compressed_data, kenney_compressed_size, 16.0f, &iconCfg);
    if (iconFont == nullptr || textFont == nullptr) {
        Log::print<ERROR>("Failed to load custom fonts for ImGui overlay, using default font");
    }
    else {
        ImGui::GetIO().Fonts->Build();
    }

    SetupImGuiStyle();

    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImPlot::CreateContext();

    VkQueue queue = nullptr;
    VRManager::instance().VK->GetDeviceDispatch()->GetDeviceQueue(VRManager::instance().VK->GetDevice(), 0, 0, &queue);

    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
        { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 }
    };

    VkDescriptorPoolCreateInfo poolInfo = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1000,
        .poolSizeCount = std::size(poolSizes),
        .pPoolSizes = poolSizes
    };
    checkVkResult(VRManager::instance().VK->GetDeviceDispatch()->CreateDescriptorPool(VRManager::instance().VK->GetDevice(), &poolInfo, nullptr, &m_descriptorPool), "Failed to create descriptor pool for ImGui");

    // create render pass to render imgui to a texture which we'll copy to the swapchain
    VkAttachmentDescription colorAttachment = {
        .format = fbFormat,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL
    };

    VkAttachmentReference colorAttachmentRef = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_GENERAL
    };

    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &colorAttachmentRef
    };

    VkSubpassDependency dependency = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .dependencyFlags = 0
    };

    VkRenderPassCreateInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &colorAttachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency
    };
    checkVkResult(VRManager::instance().VK->GetDeviceDispatch()->CreateRenderPass(VRManager::instance().VK->GetDevice(), &renderPassInfo, nullptr, &m_renderPass), "Failed to create render pass for ImGui");

    // // initialize imgui for vulkan
    ImGui::GetIO().DisplaySize = ImVec2((float)fbRes.width, (float)fbRes.height);

    // load vulkan functions
    //
    // Linux quirk: instance-level functions (e.g. vkGetPhysicalDeviceMemoryProperties)
    // returned by GetInstanceProcAddr go through the loader's terminator,
    // which validates VkPhysicalDevice against its registered list. Since we
    // hand ImGui the LAYER-side (vkroots-unwrapped) physicalDevice, the loader
    // rejects it with VUID-...-physicalDevice-parameter and aborts.
    //
    // Route the instance-level functions ImGui needs directly through the
    // vkroots dispatch table — those calls take the layer-side handle and
    // skip the loader's validation terminator.
    checkAssert(ImGui_ImplVulkan_LoadFunctions(VRManager::instance().vkVersion, [](const char* funcName, void* data_queue) {
        // Intercepts for instance-level fns whose handles fail loader validation.
        if (std::strcmp(funcName, "vkGetPhysicalDeviceMemoryProperties") == 0) {
            static auto wrap = +[](VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties* p) {
                VRManager::instance().VK->GetPhysicalDeviceDispatch()->GetPhysicalDeviceMemoryProperties(pd, p);
            };
            return (PFN_vkVoidFunction)wrap;
        }
        if (std::strcmp(funcName, "vkGetPhysicalDeviceProperties") == 0) {
            static auto wrap = +[](VkPhysicalDevice pd, VkPhysicalDeviceProperties* p) {
                VRManager::instance().VK->GetPhysicalDeviceDispatch()->GetPhysicalDeviceProperties(pd, p);
            };
            return (PFN_vkVoidFunction)wrap;
        }
        VkInstance instance = VRManager::instance().VK->GetInstance();
        VkDevice device = VRManager::instance().VK->GetDevice();
        PFN_vkVoidFunction addr = VRManager::instance().VK->GetDeviceDispatch()->GetDeviceProcAddr(device, funcName);

        if (addr == nullptr) {
            addr = VRManager::instance().VK->GetInstanceDispatch()->GetInstanceProcAddr(instance, funcName);
            Log::print<VERBOSE>("Loaded function {} at {} using instance", funcName, (void*)addr);
        }
        else {
            Log::print<VERBOSE>("Loaded function {} at {}", funcName, (void*)addr);
        }

        return addr;
    }), "Failed to load vulkan functions for ImGui");

    ImGui_ImplVulkan_InitInfo init_info = {
        .Instance = VRManager::instance().VK->GetInstance(),
        .PhysicalDevice = VRManager::instance().VK->GetPhysicalDevice(),
        .Device = VRManager::instance().VK->GetDevice(),
        .QueueFamily = 0,
        .Queue = queue,
        .DescriptorPool = m_descriptorPool,
        .RenderPass = m_renderPass,
        .MinImageCount = 6,
        .ImageCount = 6,
        .MSAASamples = VK_SAMPLE_COUNT_1_BIT,

        .UseDynamicRendering = false,

        .Allocator = nullptr,
        .CheckVkResultFn = nullptr,
        .MinAllocationSize = 1024 * 1024
    };
    checkAssert(ImGui_ImplVulkan_Init(&init_info), "Failed to initialize ImGui");

    auto* renderer = VRManager::instance().XR->GetRenderer();
    for (int i = 0; i < 2; ++i) {
        renderer->GetFrame(i).imguiFramebuffer = std::make_unique<VulkanFramebuffer>(fbRes.width, fbRes.height, fbFormat, m_renderPass);
    }

    Log::print<VERBOSE>("Initializing font textures for ImGui...");
    ImGui_ImplVulkan_CreateFontsTexture();

    // create sampler
    VkSamplerCreateInfo samplerInfo = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.minLod = -1000.0f;
    samplerInfo.maxLod = 1000.0f;
    checkVkResult(VRManager::instance().VK->GetDeviceDispatch()->CreateSampler(VRManager::instance().VK->GetDevice(), &samplerInfo, nullptr, &m_sampler), "Failed to create sampler for ImGui");

    for (int i = 0; i < 2; ++i) {
        auto& frame = renderer->GetFrame(i);

        frame.mainFramebuffer = std::make_unique<VulkanTexture>(fbRes.width, fbRes.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
        frame.mainFramebuffer->vkTransitionLayout(cb, VK_IMAGE_LAYOUT_GENERAL);
        frame.mainFramebuffer->vkClear(cb, { 0.0f, 0.0f, 0.0f, 0.0f });
        frame.mainFramebufferDS = ImGui_ImplVulkan_AddTexture(m_sampler, frame.mainFramebuffer->GetImageView(), VK_IMAGE_LAYOUT_GENERAL);

        frame.hudFramebuffer = std::make_unique<VulkanTexture>(fbRes.width, fbRes.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, false);
        frame.hudFramebuffer->vkTransitionLayout(cb, VK_IMAGE_LAYOUT_GENERAL);
        frame.hudFramebuffer->vkClear(cb, { 0.0f, 0.0f, 0.0f, 0.0f });
        frame.hudFramebufferDS = ImGui_ImplVulkan_AddTexture(m_sampler, frame.hudFramebuffer->GetImageView(), VK_IMAGE_LAYOUT_GENERAL);

        frame.hudWithoutAlphaFramebuffer = std::make_unique<VulkanTexture>(fbRes.width, fbRes.height, VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, true);
        frame.hudWithoutAlphaFramebuffer->vkTransitionLayout(cb, VK_IMAGE_LAYOUT_GENERAL);
        frame.hudWithoutAlphaFramebuffer->vkClear(cb, { 0.0f, 0.0f, 0.0f, 0.0f });
        frame.hudWithoutAlphaFramebufferDS = ImGui_ImplVulkan_AddTexture(m_sampler, frame.hudWithoutAlphaFramebuffer->GetImageView(), VK_IMAGE_LAYOUT_GENERAL);
    }

    // create VulkanTexture
    auto createHelpImage = [&](const char* title, stbi_uc const* imageData, int imageSize) {
        // load png image using stb_image
        int width, height, channels;
        unsigned char* img = stbi_load_from_memory(imageData, imageSize, &width, &height, &channels, STBI_rgb_alpha);

        // create VulkanTexture from image data
        HelpImage image;
        image.m_title = title;
        image.m_image = new VulkanTexture{ (uint32_t)width, (uint32_t)height, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        image.m_image->vkTransitionLayout(cb, VK_IMAGE_LAYOUT_GENERAL);
        image.m_image->vkUpload(cb, img, width * height * 4);
        image.m_image->vkTransitionLayout(cb, VK_IMAGE_LAYOUT_GENERAL);

        stbi_image_free(img);

        image.m_imageDS = ImGui_ImplVulkan_AddTexture(m_sampler, image.m_image->GetImageView(), VK_IMAGE_LAYOUT_GENERAL);
        return image;
    };

    m_helpImages.emplace_back(createHelpImage("Buttons & Inputs", (stbi_uc const*)controls, sizeof(controls)));
    m_helpImages.push_back(createHelpImage("Equipment", (stbi_uc const*)equip, sizeof(equip)));
    m_helpImages.push_back(createHelpImage("Swinging", (stbi_uc const*)swing, sizeof(swing)));
    m_helpImages.push_back(createHelpImage("Whistle & Magnesis", (stbi_uc const*)whistle_and_magnesis, sizeof(whistle_and_magnesis)));

    VulkanUtils::DebugPipelineBarrier(cb);

    // start the first frame right away
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}

RND_Renderer::ImGuiOverlay::~ImGuiOverlay() {
    auto* renderer = VRManager::instance().XR->GetRenderer();
    for (int i = 0; i < 2; ++i) {
        auto& frame = renderer->GetFrame(i);
        if (frame.mainFramebufferDS != VK_NULL_HANDLE)
            ImGui_ImplVulkan_RemoveTexture(frame.mainFramebufferDS);
        if (frame.mainFramebuffer != nullptr)
            frame.mainFramebuffer.reset();
        if (frame.hudFramebufferDS != VK_NULL_HANDLE)
            ImGui_ImplVulkan_RemoveTexture(frame.hudFramebufferDS);
        if (frame.hudFramebuffer != nullptr)
            frame.hudFramebuffer.reset();
        if (frame.hudWithoutAlphaFramebufferDS != VK_NULL_HANDLE)
            ImGui_ImplVulkan_RemoveTexture(frame.hudWithoutAlphaFramebufferDS);
        if (frame.hudWithoutAlphaFramebuffer != nullptr)
            frame.hudWithoutAlphaFramebuffer.reset();
        if (frame.imguiFramebuffer != nullptr)
            frame.imguiFramebuffer.reset();
    }

    for (auto& helpImage : m_helpImages) {
        ImGui_ImplVulkan_RemoveTexture(helpImage.m_imageDS);
        delete helpImage.m_image;
    }

    if (m_sampler != VK_NULL_HANDLE)
        VRManager::instance().VK->GetDeviceDispatch()->DestroySampler(VRManager::instance().VK->GetDevice(), m_sampler, nullptr);
    if (m_renderPass != VK_NULL_HANDLE)
        VRManager::instance().VK->GetDeviceDispatch()->DestroyRenderPass(VRManager::instance().VK->GetDevice(), m_renderPass, nullptr);
    if (m_descriptorPool != VK_NULL_HANDLE)
        VRManager::instance().VK->GetDeviceDispatch()->DestroyDescriptorPool(VRManager::instance().VK->GetDevice(), m_descriptorPool, nullptr);

    ImGui_ImplVulkan_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
}

static ImVec4 CalculateCenteredAspectRegion(const ImVec2& bounds, float targetAspect = 16.0f / 9.0f) {
    ImVec4 region = ImVec4(0.0f, 0.0f, bounds.x, bounds.y);

    if (bounds.x <= 0.0f || bounds.y <= 0.0f) {
        return region;
    }

    float width = bounds.x;
    float height = width / targetAspect;
    if (height > bounds.y) {
        height = bounds.y;
        width = height * targetAspect;
    }

    region = ImVec4((bounds.x - width) * 0.5f, (bounds.y - height) * 0.5f, width, height);
    return region;
}

static void TransformDrawList(ImDrawList* drawList, const ImVec2& scale, const ImVec2& offset) {
    for (auto& vert : drawList->VtxBuffer) {
        vert.pos.x = vert.pos.x * scale.x + offset.x;
        vert.pos.y = vert.pos.y * scale.y + offset.y;
    }

    for (auto& cmd : drawList->CmdBuffer) {
        cmd.ClipRect.x = cmd.ClipRect.x * scale.x + offset.x;
        cmd.ClipRect.y = cmd.ClipRect.y * scale.y + offset.y;
        cmd.ClipRect.z = cmd.ClipRect.z * scale.x + offset.x;
        cmd.ClipRect.w = cmd.ClipRect.w * scale.y + offset.y;
    }
}


void RND_Renderer::ImGuiOverlay::Update() {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::ImGuiUpdate);

    ImGui::GetIO().FontGlobalScale = 1.0f;
    auto& io = ImGui::GetIO();

#if BETTERVR_HAS_WIN32
    POINT p;
    GetCursorPos(&p);
    ScreenToClient(CemuHooks::m_cemuRenderWindow, &p);

    RECT rect;
    GetClientRect(CemuHooks::m_cemuRenderWindow, &rect);
    uint32_t windowWidth = rect.right - rect.left;
    uint32_t windowHeight = rect.bottom - rect.top;
#else
    // Linux: desktop ImGui input is driven by the OpenXR controller mapping
    // (via XR actions). Cemu's window size isn't queried here — we use the
    // output framebuffer dims directly.
    struct LinuxPoint { long x = 0, y = 0; } p;
    uint32_t windowWidth = m_outputRes.width;
    uint32_t windowHeight = m_outputRes.height;
#endif

    float fbAspect = (float)m_outputRes.width / (float)m_outputRes.height;

    // calculate the area within the window that Cemu uses to display the VR framebuffer
    uint32_t renderPhysicalWidth = windowWidth;
    uint32_t renderPhysicalHeight = windowHeight;
    if ((float)windowWidth / (float)windowHeight > fbAspect) {
        renderPhysicalWidth = (uint32_t)(windowHeight * fbAspect);
    }
    else {
        renderPhysicalHeight = (uint32_t)(windowWidth / fbAspect);
    }
    ImVec2 physicalRes = ImVec2((float)renderPhysicalWidth, (float)renderPhysicalHeight);
    ImVec2 framebufferRes = ImVec2((float)m_outputRes.width, (float)m_outputRes.height);
    ImVec4 physicalUiRegion = CalculateCenteredAspectRegion(physicalRes);
    ImVec4 framebufferUiRegion = CalculateCenteredAspectRegion(framebufferRes);

    // keep a shared 16:9 canvas with a 1080p minimum menu size
    float uiScaleFactor = std::max(physicalUiRegion.w / 1080.0f, 1.0f);

    uiScaleFactor *= 1.85f;

    ImVec2 logicalRes = ImVec2(framebufferUiRegion.z / uiScaleFactor, framebufferUiRegion.w / uiScaleFactor);

    io.DisplaySize = logicalRes;

    uint32_t blackBarWidth = (windowWidth - renderPhysicalWidth) / 2;
    uint32_t blackBarHeight = (windowHeight - renderPhysicalHeight) / 2;

    io.DisplayFramebufferScale = framebufferRes / logicalRes;

    ImVec2 physicalMenuScale = ImVec2(physicalUiRegion.z / logicalRes.x, physicalUiRegion.w / logicalRes.y);

    // map desktop cursor input into the centered 16:9 menu region
    p.x = p.x - blackBarWidth - (LONG)physicalUiRegion.x;
    p.y = p.y - blackBarHeight - (LONG)physicalUiRegion.y;

#if BETTERVR_HAS_WIN32
    bool isWindowFocused = CemuHooks::m_cemuTopWindow == GetForegroundWindow();
    if (isWindowFocused) {
        io.AddMousePosEvent((float)p.x / physicalMenuScale.x, (float)p.y / physicalMenuScale.y);
        io.AddMouseButtonEvent(0, GetAsyncKeyState(VK_LBUTTON) & 0x8000);
        io.AddMouseButtonEvent(1, GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        io.AddMouseButtonEvent(2, GetAsyncKeyState(VK_MBUTTON) & 0x8000);
    }
#else
    // Linux: input handled by XR actions.
    bool isWindowFocused = false;
#endif

    bool isWindowFocusedAndMenuOpen = isWindowFocused && VRManager::instance().XR->m_isMenuOpen;
    if (isWindowFocusedAndMenuOpen && GetSettings().IsDebuggingToolsEnabled()) {
        VRManager::instance().Hooks->m_entityDebugger->UpdateKeyboardControls();
    }
}

void RND_Renderer::ImGuiOverlay::Render(long frameIdx, bool renderBackground, bool isDesktopView) {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::ImGuiRender);

    auto* renderer = VRManager::instance().XR->GetRenderer();
    auto& frame = renderer->GetFrame(frameIdx);

    ImVec2 windowSize = ImGui::GetIO().DisplaySize;
    ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList();

    auto renderHUDBackground = [&](bool withAlpha, bool centerTo16x9) {
        ImVec2 hudPos = ImVec2(0, 0);
        ImVec2 hudSize = windowSize;

        if (centerTo16x9) {
            ImVec2 framebufferRes = ImVec2((float)m_outputRes.width, (float)m_outputRes.height);
            ImVec4 framebufferUiRegion = CalculateCenteredAspectRegion(framebufferRes);
            ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;

            hudPos = ImVec2(framebufferUiRegion.x / fbScale.x, framebufferUiRegion.y / fbScale.y);
            hudSize = ImVec2(framebufferUiRegion.z / fbScale.x, framebufferUiRegion.w / fbScale.y);
        }

        backgroundDrawList->AddImage((ImTextureID)(withAlpha ? frame.hudFramebufferDS : frame.hudWithoutAlphaFramebufferDS), hudPos, ImVec2(hudPos.x + hudSize.x, hudPos.y + hudSize.y));
    };

    auto renderMainBackground = [&](bool centerToRecordedAspect) {
        ImVec2 bgPos = ImVec2(0, 0);
        ImVec2 bgSize = windowSize;
        ImVec2 uvMin = ImVec2(frame.mainFramebufferUvTransform.offsetX, frame.mainFramebufferUvTransform.offsetY);
        ImVec2 uvMax = ImVec2(frame.mainFramebufferUvTransform.offsetX + frame.mainFramebufferUvTransform.scaleX, frame.mainFramebufferUvTransform.offsetY + frame.mainFramebufferUvTransform.scaleY);

        if (centerToRecordedAspect) {
            ImVec2 framebufferRes = ImVec2((float)m_outputRes.width, (float)m_outputRes.height);
            float targetAspect = frame.mainFramebufferAspectRatio;
            if (!(targetAspect > 0.0f)) {
                targetAspect = 16.0f / 9.0f;
            }

            ImVec4 centeredRegion = CalculateCenteredAspectRegion(framebufferRes, targetAspect);
            ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;

            bgPos = ImVec2(centeredRegion.x / fbScale.x, centeredRegion.y / fbScale.y);
            bgSize = ImVec2(centeredRegion.z / fbScale.x, centeredRegion.w / fbScale.y);
        }

        backgroundDrawList->AddImage((ImTextureID)frame.mainFramebufferDS, bgPos, ImVec2(bgPos.x + bgSize.x, bgPos.y + bgSize.y), uvMin, uvMax);
    };

    if (renderBackground || CemuHooks::UseBlackBarsDuringEvents()) {
        bool shouldRender3DBackground = VRManager::instance().XR->GetRenderer()->IsRendering3D(frameIdx) || CemuHooks::UseBlackBarsDuringEvents();
        bool shouldRenderHUDWithAlpha = shouldRender3DBackground && !CemuHooks::UseBlackBarsDuringEvents();

        if (shouldRender3DBackground) {
            // draw 3d first so the hud can blend on top of it
            renderMainBackground(isDesktopView);
        }

        renderHUDBackground(shouldRenderHUDWithAlpha, true);
    }
    else {
        renderHUDBackground(VRManager::instance().XR->GetRenderer()->IsRendering3D(frameIdx), false);
    }

    ImGuiMenus::DrawWeaponSensitivityOverlays();

    if (((renderBackground && GetSettings().performanceOverlay == PerformanceOverlayMode::WINDOW_ONLY) || GetSettings().performanceOverlay == PerformanceOverlayMode::WINDOW_AND_VR || GetSettings().performanceOverlay == PerformanceOverlayMode::WINDOW_AND_VR_WITH_PROFILER) && !VRManager::instance().XR->m_isMenuOpen) {
        ImGuiMenus::DrawFPSOverlay(renderer);
    }

    DrawHelpMenu();
}

void RND_Renderer::ImGuiOverlay::ProcessInputs(OpenXR::InputState& inputs, const VPADStatus& vpadStatus) {
    auto& isMenuOpen = VRManager::instance().XR->m_isMenuOpen;
    if (!isMenuOpen)
        return;

    auto& io = ImGui::GetIO();
    bool inGame = inputs.shared.in_game;

    bool backDown;
    bool confirmDown;
    bool pageLeft;
    bool pageRight;
    XrActionStateVector2f stick;
    if (inputs.shared.in_game) {
        stick = inputs.inGame.move;
        backDown = inputs.inGame.jump_cancel.currentState;
        confirmDown = inputs.inGame.run_interact.currentState;
        pageLeft = inputs.inGame.useLeftItem.currentState && inputs.inGame.useLeftItem.changedSinceLastSync;
        pageRight = inputs.inGame.useRightItem.currentState && inputs.inGame.useRightItem.changedSinceLastSync;

    }
    else {
        stick = inputs.inMenu.navigate;
        backDown = inputs.inMenu.back.currentState;
        confirmDown = inputs.inMenu.select.currentState;
        pageLeft = inputs.inMenu.leftTrigger.currentState && inputs.inMenu.leftTrigger.changedSinceLastSync;
        pageRight = inputs.inMenu.rightTrigger.currentState && inputs.inMenu.rightTrigger.changedSinceLastSync;
    }

    uint32_t hold = vpadStatus.hold.getLE();
    backDown |= (hold & VPAD_BUTTON_B) != 0;
    confirmDown |= (hold & VPAD_BUTTON_A) != 0;

    static bool wasBDown = false;
    bool isBDown = (hold & VPAD_BUTTON_B) != 0;
    bool bPressed = isBDown && !wasBDown;
    wasBDown = isBDown;
    
    // imgui wants us to only have state changes, and we also want to refiring DPAD inputs (used for moving the menu cursor) when held down
    constexpr float THRESHOLD_PRESS = 0.5f;
    constexpr float THRESHOLD_RELEASE = 0.3f;
    constexpr double HORIZONTAL_REFIRE_DELAY = 0.5f;
    constexpr double VERTICAL_REFIRE_DELAY = 1.0f;

    static bool dpadState[4] = { false };
    static double lastRefireTime[8] = { 0.0 }; // 0-3 Dpad, 4 B, 5 A, 6 LB, 7 RB
    double currentTime = ImGui::GetTime();

    auto updateDpadState = [&](int idx, float val, bool positive) {
        bool isPressed = dpadState[idx];
        if (positive) {
            if (!isPressed && val >= THRESHOLD_PRESS) isPressed = true;
            else if (isPressed && val <= THRESHOLD_RELEASE)
                isPressed = false;
        }
        else {
            if (!isPressed && val <= -THRESHOLD_PRESS) isPressed = true;
            else if (isPressed && val >= -THRESHOLD_RELEASE)
                isPressed = false;
        }
        dpadState[idx] = isPressed;
        return isPressed;
    };

    auto applyInput = [&](ImGuiKey key, bool isPressed, float refireDelay, int idx) {
        if (isPressed) {
            if (currentTime - lastRefireTime[idx] >= refireDelay || lastRefireTime[idx] == 0.0) {
                io.AddKeyEvent(key, true);
                lastRefireTime[idx] = currentTime;
                return true;
            }
            else {
                io.AddKeyEvent(key, false);
                return false;
            }
        }
        else {
            io.AddKeyEvent(key, false);
            lastRefireTime[idx] = 0.0;
            return false;
        }
    };

    applyInput(ImGuiKey_GamepadDpadUp, updateDpadState(0, stick.currentState.y, true) || ((hold & VPAD_BUTTON_UP) != 0), VERTICAL_REFIRE_DELAY, 0);
    applyInput(ImGuiKey_GamepadDpadDown, updateDpadState(1, stick.currentState.y, false) || ((hold & VPAD_BUTTON_DOWN) != 0), VERTICAL_REFIRE_DELAY, 1);
    applyInput(ImGuiKey_GamepadDpadLeft, updateDpadState(2, stick.currentState.x, false) || ((hold & VPAD_BUTTON_LEFT) != 0), HORIZONTAL_REFIRE_DELAY, 2);
    applyInput(ImGuiKey_GamepadDpadRight, updateDpadState(3, stick.currentState.x, true) || ((hold & VPAD_BUTTON_RIGHT) != 0), HORIZONTAL_REFIRE_DELAY, 3);

    // convert B/A to ImGui gamepad face buttons
    applyInput(ImGuiKey_GamepadFaceRight, backDown, VERTICAL_REFIRE_DELAY, 4);
    applyInput(ImGuiKey_GamepadFaceDown, confirmDown, VERTICAL_REFIRE_DELAY, 5);

    // triggers for tab switching
    bool l1 = applyInput(ImGuiKey_GamepadL1, pageLeft || ((hold & VPAD_BUTTON_L) != 0), VERTICAL_REFIRE_DELAY, 6);
    bool r1 = applyInput(ImGuiKey_GamepadR1, pageRight || ((hold & VPAD_BUTTON_R) != 0), VERTICAL_REFIRE_DELAY, 7);

    const bool hasCustomAttackSensitivityTab = GetSettings().GetSwingSensitivity() == SwingSensitivity::SWING_CUSTOM;
    const bool hasDebugTab = GetSettings().enableDebuggerTools;
    auto& currTab = VRManager::instance().XR->m_currMenuTab;
    auto isTabAvailable = [&](uint8_t tab) {
        if (tab == ImGuiMenus::DEBUG_TAB) {
            return hasDebugTab;
        }
        if (tab == ImGuiMenus::CUSTOM_ATTACK_SENSITIVITY_TAB) {
            return hasCustomAttackSensitivityTab;
        }
        return true;
    };

    if (!hasDebugTab && currTab == ImGuiMenus::DEBUG_TAB) {
        currTab = ImGuiMenus::SETTINGS_TAB;
    }
    if (!hasCustomAttackSensitivityTab && currTab == ImGuiMenus::CUSTOM_ATTACK_SENSITIVITY_TAB) {
        currTab = ImGuiMenus::SETTINGS_TAB;
    }

    VRManager::instance().XR->m_forceTabChange = false;
    if (l1 || r1) {
        uint8_t prevTab = currTab;
        uint8_t nextTab = currTab;
        if (l1) {
            do {
                nextTab = (nextTab == ImGuiMenus::SETTINGS_TAB) ? ImGuiMenus::CUSTOM_ATTACK_SENSITIVITY_TAB : (nextTab - 1);
            } while (!isTabAvailable(nextTab));
        }
        if (r1) {
            do {
                nextTab = (nextTab == ImGuiMenus::CUSTOM_ATTACK_SENSITIVITY_TAB) ? ImGuiMenus::SETTINGS_TAB : (nextTab + 1);
            } while (!isTabAvailable(nextTab));
        }
        currTab = nextTab;

        if (prevTab != currTab) {
            VRManager::instance().XR->m_forceTabChange = true;
        }
    }

    // prevent exiting menu if a popup or field is being edited
    if (ImGui::IsAnyItemActive() && ImGui::IsPopupOpen(NULL, ImGuiPopupFlags_AnyPopupId + ImGuiPopupFlags_AnyPopupLevel)) {
        return;
    }


    // if no inputs or popup is open, allow closing the menu using the cancel/back button
    bool shouldClose = false;
    if (inGame) {
        if (inputs.inGame.jump_cancel.changedSinceLastSync && inputs.inGame.jump_cancel.currentState) {
            shouldClose = true;
            inputs.inGame.jump_cancel.currentState = XR_FALSE;
        }
    }
    else {
        if (inputs.inMenu.back.changedSinceLastSync && inputs.inMenu.back.currentState) {
            shouldClose = true;
        }
    }

    if (shouldClose || bPressed) {
        VRManager::instance().XR->m_isMenuOpen = false;
    }
}

void RND_Renderer::ImGuiOverlay::Draw3DLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, float aspectRatio, const RenderUtils::UvTransform& uvTransform, long frameIdx) {
    auto& frame = VRManager::instance().XR->GetRenderer()->GetFrame(frameIdx);

    frame.mainFramebuffer->vkCopyFromImage(cb, srcImage);
    frame.mainFramebufferAspectRatio = aspectRatio;
    frame.mainFramebufferUvTransform = uvTransform;
}

void RND_Renderer::ImGuiOverlay::DrawHUDLayerAsBackground(VkCommandBuffer cb, VkImage srcImage, long frameIdx) {
    auto& frame = VRManager::instance().XR->GetRenderer()->GetFrame(frameIdx);

    frame.hudFramebuffer->vkCopyFromImage(cb, srcImage);
    frame.hudWithoutAlphaFramebuffer->vkCopyFromImage(cb, srcImage);
}

void RND_Renderer::ImGuiOverlay::DrawAndCopyToImage(VkCommandBuffer cb, VkImage destImage, long frameIdx, bool isDesktopView) {
    BetterVRProfiler::Scope profile(BetterVRProfiler::Section::ImGuiDrawAndCopy);

    ImGui::Render();

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data != nullptr && isDesktopView) {
        ImDrawList* backgroundDrawList = ImGui::GetBackgroundDrawList();
        ImVec2 framebufferRes = ImVec2((float)m_outputRes.width, (float)m_outputRes.height);
        ImVec4 framebufferUiRegion = CalculateCenteredAspectRegion(framebufferRes);
        ImVec2 drawScale = ImVec2(framebufferUiRegion.z / framebufferRes.x, framebufferUiRegion.w / framebufferRes.y);
        ImVec2 drawOffset = ImVec2(framebufferUiRegion.x / draw_data->FramebufferScale.x, framebufferUiRegion.y / draw_data->FramebufferScale.y);

        for (int listIdx = 0; listIdx < draw_data->CmdListsCount; ++listIdx) {
            ImDrawList* drawList = draw_data->CmdLists[listIdx];
            if (drawList == backgroundDrawList) {
                continue;
            }

            TransformDrawList(drawList, drawScale, drawOffset);
        }
    }

    auto* dispatch = VRManager::instance().VK->GetDeviceDispatch();
    auto* renderer = VRManager::instance().XR->GetRenderer();
    auto& frame = renderer->GetFrame(frameIdx);

    frame.imguiFramebuffer->vkClear(cb, { 0.0f, 0.0f, 0.0f, 0.0f });

    // try to delete the staging buffer of the controller scheme textures if possible
    for (auto& helpImage : m_helpImages) {
        helpImage.m_image->vkTryToFinishAnyUploads(cb);
    }

    // draw imgui to framebuffer
    VkClearValue clearValue = { .color = { 0.0f, 0.0f, 0.0f, 0.0f } };
    VkRenderPassBeginInfo renderPassInfo = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = m_renderPass,
        .framebuffer = frame.imguiFramebuffer->GetFramebuffer(),
        .renderArea = {
            .offset = { 0, 0 },
            .extent = m_outputRes,
        },
        .clearValueCount = 0,
        .pClearValues = &clearValue
    };
    dispatch->CmdBeginRenderPass(cb, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    if (draw_data != nullptr) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cb);
    }

    dispatch->CmdEndRenderPass(cb);

    // copy rendered imgui to destination image
    frame.imguiFramebuffer->vkPipelineBarrier(cb);
    frame.imguiFramebuffer->vkCopyToImage(cb, destImage);
    frame.imguiFramebuffer->vkPipelineBarrier(cb);

    // prepare for next frame immediately
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
}
