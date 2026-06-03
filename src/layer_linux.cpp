// Phase 2B step 2: separate-VkInstance probe.
//
// Step 1 established that an OpenXR instance can be brought up from inside
// our Vulkan layer, but xrGetVulkanGraphicsDeviceKHR crashes when invoked
// against Cemu's hooked VkInstance — WiVRn re-enters vkEnumeratePhysicalDevices
// and something in the dispatch chain blows up.
//
// Step 2 sidesteps that: we create our OWN VkInstance specifically for the
// OpenXR runtime to use. The crash should go away because the re-entrance
// happens on a clean instance, not Cemu's layered one. Cross-instance image
// sharing (so Cemu's frames can reach the headset) is a later problem.

#include "pch.h"
#include "utils/render_utils.h"
#include "hud_shaders.h"

#include <cstdio>
#include <cmath>
#include <cstring>
#include <mutex>
#include <atomic>
#include <thread>
#include <vector>
#include <unordered_map>
#include <deque>
#include <mutex>

namespace bvr_linux {

// Captured Vulkan handles from Cemu, populated by the layer's hooks.
// Kept so we know which physical device Cemu is on (we'll want to match it
// when we later create the OpenXR-side device).
struct CapturedHandles {
    VkInstance       instance         = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice   = VK_NULL_HANDLE;
    VkDevice         device           = VK_NULL_HANDLE;
    uint32_t         graphicsQueueFamily = UINT32_MAX;
    uint32_t         graphicsQueueIndex  = 0;
    uint32_t         apiVersion       = 0;
};

// Cross-device sharing slot. On vkCreateDevice for Cemu's device, we create
// a VkImage backed by exportable memory, fill it with a known pattern, and
// stash the fd here. The OpenXR-side device imports the same fd and blits the
// contents to the swapchain instead of the animated clear color.
struct SharedImage {
    int      fd          = -1;
    size_t   memorySize  = 0;
    uint32_t width       = 1920;  // matches typical Cemu swapchain size; blit handles mismatches
    uint32_t height      = 1080;
    VkFormat format      = VK_FORMAT_B8G8R8A8_UNORM; // Wayland/X11 surfaces commonly use BGRA
    VkImage  cemuImage   = VK_NULL_HANDLE; // The VkImage we'll copy *into* (Cemu side)
};

// Pose-with-image atomic pairing (port of upstream's m_renderFrames[idx].views
// snapshot). Hook_GetRenderCamera pushes the eye's head pose onto its queue
// as BotW prepares to render that eye. The Vulkan-side capture intercept
// (InjectPreClearCapture) pops the front of the queue — the pose at the head
// is guaranteed to be the one BotW used for the upcoming draws of this eye,
// because both events are sequenced through Cemu's command stream.
//
// The XR FrameLoop reads g_capturedPose[layer][eye] for projViews[eye].pose,
// letting the OpenXR runtime async-timewarp the captured image from its
// actual render-time pose to the live display-time pose. This is the same
// trick upstream uses and is what eliminates the head-rotation jitter when
// BotW renders at a lower rate than the headset.
static std::mutex     g_capturedPoseMutex;
static std::deque<XrPosef> g_renderedPoseQueue[2];   // [eye]
static XrPosef        g_capturedPose[2][2] = {};     // [layer][eye]
static bool           g_capturedPoseValid[2][2] = {};
// Per-eye "image captured since last consume" flag. The FrameLoop only
// updates the swapchain (and uses the captured pose) when BOTH eyes have
// captured since the last consume — that matches upstream's
// Is3DComplete()-gated submit pattern, eliminating the asymmetric
// "one eye janks" flicker that arises when one eye gets a fresh
// capture but the other doesn't between two XR frames.
static bool           g_imageFresh[2] = { false, false };
// Last consumed (pose pair). Re-used when the next bothFresh consume
// hasn't happened yet — the runtime will keep showing the last released
// swapchain images alongside this pose pair.
static XrPosef        g_lastConsumedPose[2] = {};
static bool           g_lastConsumedPoseValid[2] = { false, false };

// Captured info about Cemu's actual swapchain — populated by our CreateSwapchainKHR
// hook so we know which VkImage is being presented each frame.
struct CemuSwapchain {
    VkSwapchainKHR        handle = VK_NULL_HANDLE;
    uint32_t              width  = 0;
    uint32_t              height = 0;
    VkFormat              format = VK_FORMAT_UNDEFINED;
    std::vector<VkImage>  images;
};

static CapturedHandles g_handles;
// Per-eye shared images. Index by [layer][eye]:
//   layer 0 = 3D scene (post-HDR composed)
//   layer 1 = 2D HUD/UI overlay
// 0 = left, 1 = right.
// All are exported VkImages on Cemu's VkDevice with OPAQUE_FD memory; the
// OpenXR side imports each fd.
static SharedImage     g_eyeImages[2][2];
// Legacy alias for the 3D layer — keeps existing CmdClearColorImage code path
// minimal. New 2D path uses g_eyeImages[1][eye] explicitly.
#define g_eye3D(eye) (g_eyeImages[0][eye])
#define g_eye2D(eye) (g_eyeImages[1][eye])
static CemuSwapchain   g_cemuSwap;
static std::mutex      g_handlesMutex;
static std::atomic_bool g_xrSessionAttempted{false};

// ---- Per-eye stereo capture state ------------------------------------------
// Both eyes' procDraws write into the SAME Cemu swapchain image. To capture
// per-eye content we inject a vkCmdCopyImage at vkCmdEndRendering, copying the
// just-rendered swapchain image into a per-eye target before the next eye's
// render pass overwrites it.
//
// g_currentEye is set by Hook_BeginCameraSide on the PPC thread. The Vulkan
// render thread reads it when injecting the copy. There's a small skew window
// (PPC sets eye, GPU commands queued, GPU executes copy) but BetterVR's stereo
// loop calls BeginCameraSide → procDraw → EndCameraSide → BeginCameraSide(other)
// → procDraw → EndCameraSide tightly, so the eye in flight at any vkCmdEndRendering
// targeting the swapchain is consistent.
static std::atomic<int>      g_currentEye{0};            // 0 = left, 1 = right
static std::atomic<uint32_t> g_activeSwapImageIndex{UINT32_MAX};
// "Did we already do a per-eye capture this game frame?" Reset on
// Hook_BeginCameraSide(0) — that's the start of each new stereo cycle.
// BetterVR fires multiple magic clears per game frame (2D + 3D × eye × frame
// counter), but only the FIRST one per eye has the just-rendered content.
static std::atomic<bool>     g_capturedThisFrame[2] = {};
// Per-eye FOV ACTUALLY USED for rendering (after the symmetric-FOV override
// in ApplyVRProjection). The XR FrameLoop reads these to set projViews[*].fov
// so OpenXR composes the layer with the same FOV the content was rendered with.
static std::mutex            g_renderedFovMtx;
static bool                  g_renderedFovValid[2] = {};
static XrFovf                g_renderedFov[2] = {};
static std::atomic<uint64_t> g_injectedCopies{0};
static std::atomic<uint64_t> g_injectedCopiesPerEye[2] = {};
static std::atomic<uint64_t> g_endRenderingCount{0};
static std::atomic<uint64_t> g_endRenderingSwapchainHits{0}; // subset that matched swapchain

// VkImageView → VkImage map (filled by vkCreateImageView hook). Lets us check
// whether a render pass's color attachment view points at a swapchain image.
static std::unordered_map<VkImageView, VkImage> g_viewToImage;
static std::mutex g_viewToImageMutex;

// Per-command-buffer state: at the most recent vkCmdBeginRendering* on this
// cmdbuf, did we identify a target image as belonging to a known eye?
// Values: -2 = unset, -1 = targets swap but eye unknown, 0 = left, 1 = right.
// Read in CmdEndRendering* to decide whether (and where) to inject the copy.
static std::unordered_map<VkCommandBuffer, int> g_cmdbufEye;
static std::mutex g_cmdbufEyeMutex;

// VkFramebuffer → its attachment images (for legacy vkCmdBeginRenderPass).
static std::unordered_map<VkFramebuffer, std::vector<VkImage>> g_framebufferImages;
static std::mutex g_framebufferImagesMutex;

// VkImage → eye index. Populated by detecting the BetterVR magic clear colors
// in vkCmdClearColorImage / vkCmdClearAttachments. Once an image is identified
// as "this is eye N's framebuffer," subsequent render passes targeting it
// inject the per-eye capture into g_eyeImages[N] regardless of g_currentEye.
static std::unordered_map<VkImage, int> g_imageEye; // 0 = left, 1 = right
static std::mutex g_imageEyeMutex;
static std::atomic<uint64_t> g_imageEyeIdentified[2] = {};

// VkImage → its 2D extent (width, height). Populated by vkCreateImage hook.
// We need this so InjectPreClearCapture can size its blit src rect to the
// actual image dimensions, not a guess.
struct ImageExtent2D { uint32_t w = 0, h = 0; };
static std::unordered_map<VkImage, ImageExtent2D> g_imageExtent;
static std::mutex g_imageExtentMutex;

// Has this image been rendered to since the last magic-clear capture? We only
// want to capture once per "render-then-clear" cycle. BetterVR's patches issue
// the magic clear many times per game frame (multiple eyes × multiple alpha
// values for frame-counter encoding); most of those clears happen on already-
// cleared content and shouldn't trigger a capture.
static std::unordered_map<VkImage, bool> g_imageDirty;
static std::mutex g_imageDirtyMutex;

// Diagnostic counters per Begin*/End* variant to see which path Cemu uses.
static std::atomic<uint64_t> g_beginRenderingCount{0};      // dynamic rendering
static std::atomic<uint64_t> g_beginRenderPassCount{0};     // legacy render pass

// Shared state between the OpenXR worker (publisher) and the PPC HLE hooks
// invoked from Cemu's CPU thread (consumers). Read-modify-write on the FOV
// fields is rare and tiny, so a single mutex is fine — far simpler than
// atomic<XrFovf>.
struct BvrHookState {
    std::mutex     mtx;
    uintptr_t      memoryBase = 0;        // base of emulated Wii U RAM
    bool           fovValid[2] = { false, false };
    XrFovf         fov[2] = {};
    // Per-eye pose published by RunFrameLoop's xrLocateViews. Used by
    // Hook_GetRenderCamera to apply head-tracked camera offset to BotW's
    // gameplay camera.
    bool           poseValid[2] = { false, false };
    XrPosef        pose[2] = {};
    // Per-eye pose ACTUALLY used by Hook_GetRenderCamera when BotW rendered
    // the latest frame. We feed this back as projViews[eye].pose so the
    // OpenXR runtime knows the image's render-time pose and can reproject
    // (async timewarp) to the live display-time pose. Without this, the
    // runtime would believe the captured image is fresh and not reproject,
    // causing the head-tracking lag at low game framerates.
    bool           renderedPoseValid[2] = { false, false };
    XrPosef        renderedPose[2] = {};
    float          aspect = 1.0f;         // headset render aspect (width/height)
    std::atomic<uint64_t> publishedFrames{0};
    // Counters — how often each hook actually fires from the PPC side. Useful
    // to confirm BotW reached the BetterVR-patched stereo render path.
    std::atomic<uint64_t> hitsModifyProjection{0};
    std::atomic<uint64_t> hitsGetRenderProjection{0};
    std::atomic<uint64_t> hitsLightPrePass{0};
    std::atomic<uint64_t> hitsBeginCameraSide{0};
    // QueuePresentKHR-side blit counters
    std::atomic<uint64_t> presentBlits{0};       // successful blits Cemu→shared
    std::atomic<uint64_t> presentBlitsSkipped{0}; // present arrived but skipped (no match etc.)

    bool getFov(int eye, XrFovf* out) {
        std::lock_guard<std::mutex> lk(mtx);
        if (eye < 0 || eye > 1 || !fovValid[eye]) return false;
        *out = fov[eye];
        return true;
    }
    void publishFov(const XrFovf& left, const XrFovf& right, float a) {
        std::lock_guard<std::mutex> lk(mtx);
        fov[0] = left;  fovValid[0] = true;
        fov[1] = right; fovValid[1] = true;
        aspect = a;
        publishedFrames.fetch_add(1, std::memory_order_relaxed);
    }
    void publishPose(const XrPosef& left, const XrPosef& right) {
        std::lock_guard<std::mutex> lk(mtx);
        pose[0] = left;  poseValid[0] = true;
        pose[1] = right; poseValid[1] = true;
    }
    bool getPose(int eye, XrPosef* out) {
        std::lock_guard<std::mutex> lk(mtx);
        if (eye < 0 || eye > 1 || !poseValid[eye]) return false;
        *out = pose[eye];
        return true;
    }
    void recordRenderedPose(int eye, const XrPosef& p) {
        std::lock_guard<std::mutex> lk(mtx);
        if (eye < 0 || eye > 1) return;
        renderedPose[eye] = p;
        renderedPoseValid[eye] = true;
    }
    bool getRenderedPose(int eye, XrPosef* out) {
        std::lock_guard<std::mutex> lk(mtx);
        if (eye < 0 || eye > 1 || !renderedPoseValid[eye]) return false;
        *out = renderedPose[eye];
        return true;
    }
};
static BvrHookState g_hookState;

// `PPCInterpreter_t` (full layout) is included via pch.h → cemu.h, so the
// HLE hook bodies below can read `hCPU->gpr[N]` and `hCPU->sprNew.LR`.
using osLib_registerHLEFunctionPtr_t = void (*)(const char* lib, const char* fn,
                                                void (*osFunction)(PPCInterpreter_t*));

// ----- HLE helpers ----------------------------------------------------------

// Read/write a struct from emulated Wii U RAM. The Wii U-side address space is
// 32-bit; `g_hookState.memoryBase` is the host-side base where Cemu has mapped
// that emulated RAM. The hooks pass us pointers as 32-bit offsets in gpr[].
// BE structs have user-defined operator= so they're not "trivially copyable",
// but their data layout is POD — memcpy is the right tool here. We cast through
// void* to silence -Wnontrivial-memcall.
template <typename T>
static bool ReadGameMemory(uint32_t emuPtr, T* dst) {
    if (g_hookState.memoryBase == 0 || emuPtr == 0) return false;
    std::memcpy((void*)dst, (const void*)(g_hookState.memoryBase + emuPtr), sizeof(T));
    return true;
}

template <typename T>
static bool WriteGameMemory(uint32_t emuPtr, const T* src) {
    if (g_hookState.memoryBase == 0 || emuPtr == 0) return false;
    std::memcpy((void*)(g_hookState.memoryBase + emuPtr), (const void*)src, sizeof(T));
    return true;
}

// Env-var kill switches for incremental debugging. Read once at first use.
//   BVR_DISABLE_PROJ_HOOK=1  → projection hooks become strict no-ops
//                              (handy to confirm whether matrix rewrite is
//                              the source of a visual artifact).
//   BVR_LOG_FIRST_PROJ=1     → log first observed FOV + computed matrix
//                              from ApplyVRProjection.
static bool DisableProjHook() {
    static bool cached = [](){
        const char* v = std::getenv("BVR_DISABLE_PROJ_HOOK");
        bool on = v && v[0] && v[0] != '0';
        std::fprintf(stderr, "[BetterVR-Linux] DisableProjHook=%d\n", (int)on);
        return on;
    }();
    return cached;
}
static bool LogFirstProj() {
    static bool cached = [](){
        const char* v = std::getenv("BVR_LOG_FIRST_PROJ");
        return v && v[0] && v[0] != '0';
    }();
    return cached;
}

// Common per-eye projection rewrite. Takes the game's current projection
// (already read from memory), looks up the matching eye's OpenXR FOV, and
// updates aspect/fov/matrix/deviceMatrix fields in place. Returns false if
// no OpenXR FOV is yet published (in which case we leave the game's
// projection unchanged so it can still render in mono).
static bool ApplyVRProjection(BESeadPerspectiveProjection& projection, int eye) {
    XrFovf eyeFov = {};
    if (!g_hookState.getFov(eye, &eyeFov)) return false;

    // Optionally adjust for the game's render aspect ratio. We don't have a
    // game-side aspect override yet, so feed nullopt and let ResolveGameProjectionFov
    // fall through to the raw FOV.
    std::optional<XrFovf> resolved = RenderUtils::ResolveGameProjectionFov(
        eyeFov,
        std::optional<float>{},
        projection.fovYRadiansOrAngle.getLE(),
        projection.aspect.getLE());
    if (!resolved.has_value()) return false;

    XrFovf currFOV = resolved.value();

    // BVR_SYMMETRIC_FOV=1 forces a SYMMETRIC FOV (zeroed horizontal off-axis).
    // Without proper IPD camera offset, the off-axis frustums from the
    // headset's natural per-eye FOV cause distant objects to converge to a
    // finite point — eyes have to cross to fuse them. Symmetric FOV eliminates
    // that at the cost of slightly less peripheral coverage; the per-eye
    // images become nearly identical, but the resulting comfort is well worth
    // the lost "true VR" feel until camera offset is wired up.
    static const bool kSymmetricFov = [](){
        const char* v = std::getenv("BVR_SYMMETRIC_FOV");
        bool on = v && v[0] && v[0] != '0'; // default OFF (off-axis FOV is correct for VR)
        std::fprintf(stderr, "[BetterVR-Linux] SymmetricFov=%d\n", (int)on);
        return on;
    }();
    if (kSymmetricFov) {
        float horiz_half = std::max(std::fabs(currFOV.angleLeft), std::fabs(currFOV.angleRight));
        float vert_half  = std::max(std::fabs(currFOV.angleUp),   std::fabs(currFOV.angleDown));
        currFOV.angleLeft  = -horiz_half;
        currFOV.angleRight =  horiz_half;
        currFOV.angleDown  = -vert_half;
        currFOV.angleUp    =  vert_half;
    }
    // Publish the FOV we ACTUALLY rendered with so the XR composition layer
    // uses the matching FOV. Without this, OpenXR would compose using the
    // headset's asymmetric off-axis FOV while the content was rendered with
    // our symmetric FOV → distant objects shift wrongly.
    {
        std::lock_guard<std::mutex> lk(g_renderedFovMtx);
        g_renderedFov[eye] = currFOV;
        g_renderedFovValid[eye] = true;
    }
    auto newProj = RenderUtils::CalculateFOVAndOffset(currFOV);

    if (LogFirstProj()) {
        static std::once_flag once;
        std::call_once(once, [&](){
            std::fprintf(stderr,
                "[BetterVR-Linux] First proj eye=%d  rawFov L=%.3f R=%.3f U=%.3f D=%.3f  resolved L=%.3f R=%.3f U=%.3f D=%.3f  "
                "newAspect=%.3f newFovY=%.3f offX=%.3f offY=%.3f  zN=%.3f zF=%.3f zScale=%.3f zOff=%.3f\n",
                eye,
                (double)eyeFov.angleLeft, (double)eyeFov.angleRight,
                (double)eyeFov.angleUp,   (double)eyeFov.angleDown,
                (double)currFOV.angleLeft, (double)currFOV.angleRight,
                (double)currFOV.angleUp,   (double)currFOV.angleDown,
                (double)newProj.aspectRatio.getLE(),
                (double)newProj.fovY.getLE(),
                (double)newProj.offsetX.getLE(),
                (double)newProj.offsetY.getLE(),
                (double)projection.zNear.getLE(),
                (double)projection.zFar.getLE(),
                (double)projection.deviceZScale.getLE(),
                (double)projection.deviceZOffset.getLE());
        });
    }
    projection.aspect            = newProj.aspectRatio;
    projection.fovYRadiansOrAngle = newProj.fovY;
    float halfAngle = newProj.fovY.getLE() * 0.5f;
    projection.fovySin = sinf(halfAngle);
    projection.fovyCos = cosf(halfAngle);
    projection.fovyTan = tanf(halfAngle);
    projection.offset.x = newProj.offsetX;
    projection.offset.y = newProj.offsetY;

    glm::fmat4 newMatrix = RenderUtils::CalculateProjectionMatrix(
        projection.zNear.getLE(), projection.zFar.getLE(), currFOV);
    projection.matrix = newMatrix;

    // Device matrix carries the same projection but with the game's
    // platform-specific Z scale/offset applied. Same math the upstream
    // hooks use.
    glm::fmat4 newDeviceMatrix = newMatrix;
    float zScale  = projection.deviceZScale.getLE();
    float zOffset = projection.deviceZOffset.getLE();
    newDeviceMatrix[2][0] *= zScale;
    newDeviceMatrix[2][1] *= zScale;
    newDeviceMatrix[2][2] = (newDeviceMatrix[2][2] + newDeviceMatrix[3][2] * zOffset) * zScale;
    newDeviceMatrix[2][3] =  newDeviceMatrix[2][3] * zScale + newDeviceMatrix[3][3] * zOffset;
    projection.deviceMatrix = newDeviceMatrix;

    projection.dirty       = false;
    projection.deviceDirty = false;
    return true;
}

// ----- HLE hook bodies ------------------------------------------------------
//
// These match the upstream calling convention used by the BetterVR graphic-pack
// patches (`patch_RND_StereoRendering.asm` etc.). The patches call into Cemu's
// HLE registry with `bla import.coreinit.hook_X`; Cemu invokes us with the
// current `PPCInterpreter_t*`. We must set `instructionPointer = sprNew.LR` so
// Cemu returns to the patch site instead of executing the (non-existent) BLA
// target.

// Just bookkeeping for now — upstream uses these to enter/exit per-eye
// debug-draw scopes. Logging would be too noisy at 90 Hz × 2 eyes.
static void Hook_BeginCameraSide(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    g_hookState.hitsBeginCameraSide.fetch_add(1, std::memory_order_relaxed);
    int eye = (hCPU->gpr[0] == 0) ? 0 : 1;
    g_currentEye.store(eye, std::memory_order_release);
    // Each call to BeginCameraSide marks the start of an eye's draw cycle for
    // this game frame. Reset the "captured this frame" flag for THAT eye so
    // the next magic-clear capture for it actually fires.
    g_capturedThisFrame[eye].store(false, std::memory_order_release);
}
static void Hook_EndCameraSide(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
}

// `r3` = projection-in pointer; `r12` = projection-out pointer; `r0` = eye side.
static void Hook_GetRenderProjection(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    g_hookState.hitsGetRenderProjection.fetch_add(1, std::memory_order_relaxed);
    if (DisableProjHook()) return;

    uint32_t projectionIn  = hCPU->gpr[3];
    uint32_t projectionOut = hCPU->gpr[12];
    int eye = (hCPU->gpr[0] == 0) ? 0 : 1;

    BESeadPerspectiveProjection projection = {};
    if (!ReadGameMemory(projectionIn, &projection)) return;

    if (projection.zFar == 10000.0f) {
        // Upstream skips the projection rewrite for this magic far plane —
        // matches the menu/UI projection that shouldn't get VR treatment.
        return;
    }

    if (!ApplyVRProjection(projection, eye)) return;

    WriteGameMemory(projectionOut, &projection);
    hCPU->gpr[3] = projectionOut;
}

// `r4` = projection pointer; `r5` = eye side. We intentionally do NOT touch the
// camera (`r7`) here yet — that requires the world-space pose / yaw blending
// that camera.cpp does, and we don't have a controller pose source on Linux
// yet. Per-eye projection is the minimum the StereoRendering patches need to
// stop spinning on the loading screen.
static void Hook_ModifyProjectionUsingCamera(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    g_hookState.hitsModifyProjection.fetch_add(1, std::memory_order_relaxed);
    if (DisableProjHook()) return;

    uint32_t projectionPtr = hCPU->gpr[4];
    int eye = (hCPU->gpr[5] == 0) ? 0 : 1;

    BESeadPerspectiveProjection projection = {};
    if (!ReadGameMemory(projectionPtr, &projection)) return;
    if (!ApplyVRProjection(projection, eye)) return;
    WriteGameMemory(projectionPtr, &projection);
}

// `r3` = projection pointer (in/out); `r11` = eye side.
static void Hook_ModifyLightPrePassProjectionMatrix(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    g_hookState.hitsLightPrePass.fetch_add(1, std::memory_order_relaxed);
    if (DisableProjHook()) return;

    uint32_t projectionPtr = hCPU->gpr[3];
    int eye = (hCPU->gpr[11] == 0) ? 0 : 1;

    BESeadPerspectiveProjection projection = {};
    if (!ReadGameMemory(projectionPtr, &projection)) return;
    if (!ApplyVRProjection(projection, eye)) return;
    WriteGameMemory(projectionPtr, &projection);
}

// No-op shims that just return to the patch site. These exist because the
// patches reference them but their behavior depends on world-space camera
// data we don't compute yet.
static void Hook_Noop(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
}

// ----- Tail-call hooks --------------------------------------------------------
//
// Several BetterVR patches use `bla import.coreinit.hook_X` to overwrite a BotW
// `bl <something>` call site. The C++ hook is then expected to TAIL-CALL into
// the original BotW function (set instructionPointer to its entry), not return
// to LR. Our previous Hook_Noop did the wrong thing: returning to LR makes BotW
// skip the original work entirely, leaving game state (player normals, rigid
// bodies, static params) uninitialized. Several frames later BotW dereferences
// a pointer field that should have been set by the skipped work → NULL deref →
// the world-load crash we caught in gdb. These hooks redirect to the original
// BotW function address taken from upstream camera.cpp / game_state.cpp /
// entity_controller.cpp. We don't replicate the upstream's conditional logic
// (first-person blocking etc.) — just always take the "normal" path, which is
// equivalent to "the BetterVR pack isn't doing anything here, run vanilla BotW".

#define BVR_TAIL_CALL(name, addr) \
    static void name(PPCInterpreter_t* hCPU) { hCPU->instructionPointer = (addr); }

BVR_TAIL_CALL(Hook_PlayerNormalChangeState,    0x037B8284) // orig inner-change-child
BVR_TAIL_CALL(Hook_PlayerLadderFix,            0x02D07CEC)
BVR_TAIL_CALL(Hook_SetRigidBodyTransform,      0x03486CD8)
BVR_TAIL_CALL(Hook_SetRigidBodyScale,          0x03486F50)
BVR_TAIL_CALL(Hook_SetRigidBodyPosition,       0x03486D84)
BVR_TAIL_CALL(Hook_SetRigidBodyPositionAndRotation, 0x03489A84)
BVR_TAIL_CALL(Hook_LoadDynamicVec,             0x030EC844)
BVR_TAIL_CALL(Hook_OverwriteFloatParam,        0x030E9BE0) // orig GetStaticParam_float
#undef BVR_TAIL_CALL

// Compute a head-tracked, IPD-offset camera for the given eye. Based on
// upstream's BuildGameplayCameraPose in src/hooking/camera.cpp.
// Returns world-space (position, rotation) for the eye.
static std::pair<glm::vec3, glm::fquat> ComputeVrCameraPose(
    const glm::vec3& gameplayPos,
    const glm::fquat& gameplayRot,
    int eye)
{
    // Yaw-only twist of the gameplay rotation — keeps the gameplay camera's
    // horizontal direction but discards its pitch/roll so head rotation
    // doesn't double up.
    glm::fquat baseYaw;
    {
        glm::vec3 yAxis(0.0f, 1.0f, 0.0f);
        glm::vec3 r(gameplayRot.x, gameplayRot.y, gameplayRot.z);
        float dotV = glm::dot(r, yAxis);
        glm::vec3 proj = yAxis * dotV;
        glm::fquat twist = glm::normalize(glm::fquat(gameplayRot.w, proj.x, proj.y, proj.z));
        baseYaw = twist;
    }

    XrPosef xrPose = {};
    if (!g_hookState.getPose(eye, &xrPose)) {
        return { gameplayPos, gameplayRot };
    }
    // Push the head pose we're about to render this eye with onto the per-eye
    // queue. InjectPreClearCapture pops the front when it captures this eye's
    // framebuffer — that pairs the captured image with the pose BotW used to
    // render it. The XR FrameLoop uses the resulting per-eye pose for
    // projViews[eye].pose so the runtime can async-timewarp the captured
    // image to match the live head pose at display time.
    {
        std::lock_guard<std::mutex> lk(g_capturedPoseMutex);
        g_renderedPoseQueue[eye].push_back(xrPose);
        // Cap so a missed capture (e.g., a non-rendering BotW frame) can't
        // grow the queue unboundedly. The OLDEST entry is dropped — captures
        // get the most recent unconsumed pose.
        while (g_renderedPoseQueue[eye].size() > 8) g_renderedPoseQueue[eye].pop_front();
    }
    g_hookState.recordRenderedPose(eye, xrPose);
    glm::vec3  eyePos(xrPose.position.x, xrPose.position.y, xrPose.position.z);
    glm::fquat eyeRot(xrPose.orientation.w, xrPose.orientation.x,
                      xrPose.orientation.y, xrPose.orientation.z);

    glm::vec3  newPos = gameplayPos + (baseYaw * eyePos);
    glm::fquat newRot = baseYaw * eyeRot;
    return { newPos, newRot };
}

// hook_GetRenderCamera: the patch sets r3 = input camera ptr, r11 = currentEye,
// r12 = modifiedCopy_seadLookAtCamera scratch. We're expected to read the input
// camera, apply VR adjustments (head rotation + IPD offset per eye), write the
// new camera to the scratch, and return gpr[3] = scratch+4.
//
// Without this implementation BotW's gameplay camera angle was applied to BOTH
// eyes identically, so head rotation didn't reveal new parts of the world.
static void Hook_GetRenderCamera(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;

    uint32_t cameraIn  = hCPU->gpr[3];
    uint32_t cameraOut = hCPU->gpr[12];
    int eye = (hCPU->gpr[11] == 0) ? 0 : 1;

    if (cameraIn == 0 || cameraOut == 0) return;

    BESeadLookAtCamera camera = {};
    if (!ReadGameMemory(cameraIn, &camera)) return;

    // Extract gameplay base pose from the input camera's view matrix.
    glm::mat4x3 viewMatrix = camera.mtx.getLEMatrix();
    glm::mat4 worldGame = glm::inverse(glm::mat4(viewMatrix));
    glm::vec3  gameplayPos = glm::vec3(worldGame[3]);
    glm::fquat gameplayRot = glm::quat_cast(worldGame);

    auto [newPos, newRot] = ComputeVrCameraPose(gameplayPos, gameplayRot, eye);

    glm::mat4 newWorld = glm::translate(glm::mat4(1.0f), newPos) * glm::mat4_cast(newRot);
    glm::mat4 newView  = glm::inverse(newWorld);

    camera.mtx.setLEMatrix(glm::mat4x3(newView));
    camera.pos = newPos;
    // Look-at point: position + forward direction (-Z in view space).
    glm::vec3 viewDir = -glm::vec3(newView[2]);
    camera.at = newPos + viewDir;
    // Up direction (+Y in view space).
    camera.up = glm::vec3(newView[1]);

    WriteGameMemory(cameraOut, &camera);
    hCPU->gpr[3] = cameraOut;
}

// patch_CTRL_ButtonInput.asm wraps VPADRead: after the real VPADRead returns,
// it calls hook_InjectXRInput. If we return r3=1, the patch zeroes the error
// pointer and reports success to BotW, clearing the "can't communicate with
// Wii U gamepad" warning. We don't actually overlay XR controller input yet
// (needs an XR action set / xrSyncActions plumbed in); the existing VPADStatus
// populated by the original VPADRead drives input through Cemu's regular
// keyboard/gamepad mapping.
static void Hook_InjectXRInput(PPCInterpreter_t* hCPU) {
    hCPU->instructionPointer = hCPU->sprNew.LR;
    hCPU->gpr[3] = 1;
}

static void RegisterHLEHooks() {
    auto h = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return;
    auto reg = (osLib_registerHLEFunctionPtr_t)dlsym(h, "osLib_registerHLEFunction");
    if (!reg) {
        std::fprintf(stderr, "[BetterVR-Linux] HLE: osLib_registerHLEFunction not found\n");
        return;
    }
    struct Entry { const char* name; void (*fn)(PPCInterpreter_t*); };
    static const Entry kHooks[] = {
        // Stereo rendering core — real bodies
        { "hook_BeginCameraSide",                 &Hook_BeginCameraSide },
        { "hook_EndCameraSide",                   &Hook_EndCameraSide },
        { "hook_ModifyProjectionUsingCamera",     &Hook_ModifyProjectionUsingCamera },
        { "hook_ModifyLightPrePassProjectionMatrix", &Hook_ModifyLightPrePassProjectionMatrix },
        { "hook_GetRenderProjection",             &Hook_GetRenderProjection },
        // Input — minimal: signals "I handled the input" so patch clears the
        // BotW gamepad warning, but doesn't actually overlay XR controller
        // input. Cemu's normal keyboard/gamepad mapping still drives BotW.
        { "hook_InjectXRInput",                   &Hook_InjectXRInput },
        // Camera + visibility — no-op shims for now (require pose/visibility
        // math we haven't ported). Patches still reference these so they must
        // be registered.
        { "hook_OverwriteSeadPerspectiveProjectionSet", &Hook_Noop },
        { "hook_GetRenderCamera",                 &Hook_GetRenderCamera },
        { "hook_UpdateCameraForGameplay",         &Hook_Noop },
        { "hook_AdjustGameplayCameraPivot",       &Hook_Noop },
        { "hook_CheckIfCameraCanSeePos",          &Hook_Noop },
        { "hook_RouteActorJob",                   &Hook_Noop },
        // ----- Bulk no-op registrations for every other hook referenced by
        // the BetterVR graphic-pack patches. Without these, Cemu's HLE table
        // has no entry for the call sites — what happens then depends on
        // Cemu's unknown-HLE-import handling, which is risky during heavy
        // transitions (e.g. world loads). Explicit no-ops give predictable
        // behavior: each one just sets PC = LR and returns. They are listed
        // grouped by patch file for readability.
        // patch_CTRL_Bones.asm
        { "hook_ModifyBoneMatrix",                &Hook_Noop },
        // patch_CTRL_EntityController.asm (roomscale)
        { "hook_PrepareRoomscaleRaycast",         &Hook_Noop },
        { "hook_ConsumeRoomscaleRaycast",         &Hook_Noop },
        { "hook_BeginRoomscaleMovement",          &Hook_Noop },
        { "hook_BuildRoomscaleWarpTransform",     &Hook_Noop },
        { "hook_TestPlayerSetMtxTeleport",        &Hook_Noop },
        { "hook_VisualizeRayCastHits",            &Hook_Noop },
        // patch_CTRL_Equipment.asm / patch_CTRL_WeaponAttacks.asm
        { "hook_DropEquipment",                   &Hook_Noop },
        { "hook_DropWeaponLogging",               &Hook_Noop },
        { "hook_EquipWeapon",                     &Hook_Noop },
        { "hook_ChangeWeaponMtx",                 &Hook_Noop },
        { "hook_EnableWeaponAttackSensor",        &Hook_Noop },
        { "hook_GetContactLayerOfAttack",         &Hook_Noop },
        { "hook_SetPlayerWeaponScale",            &Hook_Noop },
        // patch_CTRL_HookWeaponHands.asm
        { "hook_ModifyHandModelAccessSearch",     &Hook_Noop },
        // patch_CTRL_NewActorHook.asm
        { "hook_CreateNewActor",                  &Hook_Noop },
        { "hook_UpdateActorList",                 &Hook_Noop },
        // patch_CTRL_Rumble.asm
        { "hook_XRRumble",                        &Hook_Noop },
        // patch_FirstPersonMode.asm
        { "hook_CalculateModelOpacity",           &Hook_Noop },
        { "hook_SetActorOpacity",                 &Hook_Noop },
        { "hook_UseCameraDistance",               &Hook_Noop },
        // patch_FirstPersonMode_Events.asm
        { "hook_ReplaceCameraMode",               &Hook_Noop },
        { "hook_ShouldSkipEventCamera",           &Hook_Noop },
        { "hook_GetEventName",                    &Hook_Noop },
        // patch_FixVisibilityChecks.asm
        { "hook_PlayerIsRiding",                  &Hook_Noop },
        { "hook_PlayerIsRidingSandSeal",          &Hook_Noop },
        // patch_ImproveGUI.asm
        { "hook_FixUIBlending",                   &Hook_Noop },
        { "hook_FixExtraStaminaGaugeIconPositions", &Hook_Noop },
        { "hook_FixStaminaGaugeScreenPosition",   &Hook_Noop },
        { "hook_CreateNewScreen",                 &Hook_Noop },
        // patch_Misc.asm — these are tail-call hooks (patches use `bla`, the C++
        // hook is expected to redirect to the original BotW function entry, not
        // return to LR). Returning to LR skips BotW's work, leaving state
        // uninitialized and causing NULL-pointer crashes later in world load.
        { "hook_PlayerNormalChangeState",         &Hook_PlayerNormalChangeState },
        { "hook_PlayerLadderFix",                 &Hook_PlayerLadderFix },
        { "hook_SetRigidBodyPosition",            &Hook_SetRigidBodyPosition },
        { "hook_SetRigidBodyPositionAndRotation", &Hook_SetRigidBodyPositionAndRotation },
        { "hook_SetRigidBodyScale",               &Hook_SetRigidBodyScale },
        { "hook_SetRigidBodyTransform",           &Hook_SetRigidBodyTransform },
        // Other patch_Misc.asm hooks — no tail-call address documented upstream,
        // keep as no-op for now (may still need fixing).
        { "hook_FixCameraSaveFilesAndInventory",  &Hook_Noop },
        { "hook_FixLadder",                       &Hook_Noop },
        { "hook_RemoveRagdollControllerFromWorld", &Hook_Noop },
        { "hook_SetRagdollControllerScale",       &Hook_Noop },
        { "hook_SetRagdollControllerTransform",   &Hook_Noop },
        { "hook_SetRigidBodyVelocity",            &Hook_Noop },
        // patch_RND_StereoRendering_Optimizations.asm — hook_OverwriteFloatParam
        // and hook_LoadDynamicVec are tail-call hooks too (30+ patch sites for
        // OverwriteFloatParam, every one of them needs correct redirect).
        { "hook_OverwriteFloatParam",             &Hook_OverwriteFloatParam },
        { "hook_LoadDynamicVec",                  &Hook_LoadDynamicVec },
        { "hook_ModifyPixelUniformBlockData",     &Hook_Noop },
        { "hook_LoadDynamicBool",                 &Hook_Noop },
        // patch_Settings.asm
        { "hook_UpdateSettings",                  &Hook_Noop },
        // patch_debug_*.asm
        { "hook_OSReportToConsole",               &Hook_Noop },
        { "hook_ProfileSectionBegin",             &Hook_Noop },
        { "hook_ProfileSectionEnd",               &Hook_Noop },
    };
    constexpr size_t kRealCount = 6;
    for (const auto& e : kHooks) {
        reg("coreinit", e.name, e.fn);
    }
    std::fprintf(stderr, "[BetterVR-Linux] HLE: registered %zu hooks (%zu real, %zu no-op)\n",
                 sizeof(kHooks) / sizeof(kHooks[0]),
                 kRealCount,
                 sizeof(kHooks) / sizeof(kHooks[0]) - kRealCount);
}

// Probe Cemu's exported symbols once.
static bool ProbeCemuExports() {
    void* h = dlopen(nullptr, RTLD_NOW | RTLD_GLOBAL);
    if (!h) return false;
    void* p1 = dlsym(h, "gameMeta_getTitleId");
    void* p2 = dlsym(h, "memory_getBase");
    void* p3 = dlsym(h, "osLib_registerHLEFunction");
    std::fprintf(stderr,
        "[BetterVR-Linux] Cemu exports: titleId=%p memBase=%p registerHLE=%p\n",
        p1, p2, p3);

    // Capture the emulated-RAM base pointer once. The HLE hooks need it to
    // dereference game-side struct pointers passed via gpr[].
    if (p2 && g_hookState.memoryBase == 0) {
        auto getBase = (void* (*)())p2;
        void* base = getBase();
        if (base) {
            g_hookState.memoryBase = (uintptr_t)base;
            std::fprintf(stderr, "[BetterVR-Linux] HLE: memory base = %p\n", base);
        }
    }

    // Register the BetterVR-graphic-pack hooks once we know Cemu's
    // registration API is available.
    static std::once_flag once;
    if (p3) std::call_once(once, RegisterHLEHooks);
    return p1 && p2 && p3;
}

// Bootstraps Vulkan loader entry points via dlsym. With VK_NO_PROTOTYPES set
// in pch.h, vulkan.h doesn't declare the loader functions directly.
struct VkBoot {
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkCreateInstance      CreateInstance      = nullptr;
    PFN_vkDestroyInstance     DestroyInstance     = nullptr;
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
    PFN_vkGetDeviceProcAddr   GetDeviceProcAddr   = nullptr;
};

// Fill out our SharedImage on the Cemu side: create a VkImage backed by
// exportable memory, fill it with a known gradient, export the fd. The
// OpenXR worker thread will import the same fd later.
//
// Must be called from a worker thread AFTER vkroots has populated its
// VkDeviceDispatch for the device (i.e. after our CreateDevice override
// returns). We dispatch this from QueuePresentKHR's worker.
static void SetupSharedImageForEye(int layer, int eye,
                                   VkInstance instance, VkPhysicalDevice physDev,
                                   VkDevice device, uint32_t queueFamily,
                                   const VkBoot& vb)
{
    const vkroots::VkDeviceDispatch* dd = vkroots::tables::DeviceDispatches.find(device);
    const vkroots::VkInstanceDispatch* id = vkroots::tables::InstanceDispatches.find(instance);
    if (!dd || !id) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: vkroots dispatch missing (dd=%p id=%p)\n",
                     layer, eye, (const void*)dd, (const void*)id);
        return;
    }
    SharedImage& target = g_eyeImages[layer][eye];

    // 1) Create the VkImage with external-memory specifier
    VkExternalMemoryImageCreateInfo extImg = {};
    extImg.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkImageCreateInfo ici = {};
    ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext         = &extImg;
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = target.format;
    ici.extent        = { target.width, target.height, 1 };
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (dd->CreateImage(device, &ici, nullptr, &image) != VK_SUCCESS) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: vkCreateImage failed\n", layer, eye);
        return;
    }

    VkMemoryRequirements req = {};
    dd->GetImageMemoryRequirements(device, image, &req);

    // 2) Pick a DEVICE_LOCAL memory type compatible with the requirements
    VkPhysicalDeviceMemoryProperties memProps = {};
    id->GetPhysicalDeviceMemoryProperties(physDev, &memProps);
    uint32_t memTypeIdx = UINT32_MAX;
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((req.memoryTypeBits & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            memTypeIdx = i;
            break;
        }
    }
    if (memTypeIdx == UINT32_MAX) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: no compatible memory type\n", layer, eye);
        return;
    }

    // 3) Allocate exportable memory
    VkExportMemoryAllocateInfo exportInfo = {};
    exportInfo.sType       = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
    exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkMemoryAllocateInfo mai = {};
    mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext           = &exportInfo;
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = memTypeIdx;

    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (dd->AllocateMemory(device, &mai, nullptr, &memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: vkAllocateMemory failed\n", layer, eye);
        return;
    }
    if (dd->BindImageMemory(device, image, memory, 0) != VK_SUCCESS) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: vkBindImageMemory failed\n", layer, eye);
        return;
    }

    // 4) Fill the image with a clearly recognisable color so we can tell if
    // the OpenXR side is displaying an empty/uninitialized eye. Left eye =
    // cyan, right eye = magenta — visible distinction during testing.
    VkQueue queue = VK_NULL_HANDLE;
    dd->GetDeviceQueue(device, queueFamily, 0, &queue);

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.queueFamilyIndex = queueFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    dd->CreateCommandPool(device, &cpci, nullptr, &pool);

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool        = pool;
    cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    dd->AllocateCommandBuffers(device, &cbai, &cmd);

    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    dd->BeginCommandBuffer(cmd, &cbbi);

    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier toDst = {};
    toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask       = 0;
    toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image               = image;
    toDst.subresourceRange    = range;
    dd->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &toDst);

    VkClearColorValue clearColor = {};
    if (eye == 0) { clearColor.float32[0] = 0.0f; clearColor.float32[1] = 0.9f; clearColor.float32[2] = 1.0f; }
    else          { clearColor.float32[0] = 1.0f; clearColor.float32[1] = 0.0f; clearColor.float32[2] = 0.9f; }
    clearColor.float32[3] = 1.0f;
    dd->CmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       &clearColor, 1, &range);

    // Leave the image in TRANSFER_SRC_OPTIMAL so the OpenXR side can blit out of it.
    VkImageMemoryBarrier toSrc = {};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = image;
    toSrc.subresourceRange    = range;
    dd->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                       0, nullptr, 0, nullptr, 1, &toSrc);

    dd->EndCommandBuffer(cmd);

    VkSubmitInfo si = {};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cmd;
    dd->QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    dd->DeviceWaitIdle(device);

    // 5) Export the fd
    VkMemoryGetFdInfoKHR fdInfo = {};
    fdInfo.sType      = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
    fdInfo.memory     = memory;
    fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    int fd = -1;
    if (dd->GetMemoryFdKHR(device, &fdInfo, &fd) != VK_SUCCESS || fd < 0) {
        std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: vkGetMemoryFdKHR failed\n", layer, eye);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(g_handlesMutex);
        target.fd         = fd;
        target.memorySize = req.size;
        target.cemuImage  = image;
    }
    std::fprintf(stderr, "[BetterVR-Linux] Shared[L%d/E%d]: exported %ux%u image, fd=%d size=%zu cemuImage=%p\n",
                 layer, eye, target.width, target.height, fd, (size_t)req.size, (void*)image);
}

// Convenience: set up all 4 eye images (3D L/R + 2D L/R).
static void SetupSharedImageFromCemuDevice(VkInstance instance, VkPhysicalDevice physDev,
                                           VkDevice device, uint32_t queueFamily,
                                           const VkBoot& vb)
{
    for (int layer = 0; layer < 2; ++layer) {
        for (int eye = 0; eye < 2; ++eye) {
            SetupSharedImageForEye(layer, eye, instance, physDev, device, queueFamily, vb);
        }
    }
}

// Device-level Vulkan functions, resolved from the OpenXR-side VkDevice.
struct VkDeviceFns {
    PFN_vkGetDeviceQueue       GetDeviceQueue       = nullptr;
    PFN_vkCreateCommandPool    CreateCommandPool    = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer   BeginCommandBuffer   = nullptr;
    PFN_vkEndCommandBuffer     EndCommandBuffer     = nullptr;
    PFN_vkResetCommandBuffer   ResetCommandBuffer   = nullptr;
    PFN_vkCmdPipelineBarrier   CmdPipelineBarrier   = nullptr;
    PFN_vkCmdClearColorImage   CmdClearColorImage   = nullptr;
    PFN_vkCmdBlitImage         CmdBlitImage         = nullptr;
    PFN_vkQueueSubmit          QueueSubmit          = nullptr;
    PFN_vkDeviceWaitIdle       DeviceWaitIdle       = nullptr;
    PFN_vkCreateImage          CreateImage          = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory       AllocateMemory       = nullptr;
    PFN_vkBindImageMemory      BindImageMemory      = nullptr;
    // HUD overlay pipeline (added for 2D HUD compositing).
    PFN_vkCreateShaderModule   CreateShaderModule   = nullptr;
    PFN_vkCreateSampler        CreateSampler        = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateImageView      CreateImageView      = nullptr;
    PFN_vkCmdBeginRendering    CmdBeginRendering    = nullptr;
    PFN_vkCmdEndRendering      CmdEndRendering      = nullptr;
    PFN_vkCmdBindPipeline      CmdBindPipeline      = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdSetViewport       CmdSetViewport       = nullptr;
    PFN_vkCmdSetScissor        CmdSetScissor        = nullptr;
    PFN_vkCmdPushConstants     CmdPushConstants     = nullptr;
    PFN_vkCmdDraw              CmdDraw              = nullptr;

    bool Resolve(VkDevice dev, PFN_vkGetDeviceProcAddr gdpa) {
#define R(F) F = (PFN_vk##F)gdpa(dev, "vk" #F); if (!F) return false
        R(GetDeviceQueue);
        R(CreateCommandPool);
        R(AllocateCommandBuffers);
        R(BeginCommandBuffer);
        R(EndCommandBuffer);
        R(ResetCommandBuffer);
        R(CmdPipelineBarrier);
        R(CmdClearColorImage);
        R(CmdBlitImage);
        R(QueueSubmit);
        R(DeviceWaitIdle);
        R(CreateImage);
        R(GetImageMemoryRequirements);
        R(AllocateMemory);
        R(BindImageMemory);
#undef R
        // HUD pipeline functions: optional (HUD won't render if missing,
        // pipeline build sees nullptrs and short-circuits).
#define O(F) F = (PFN_vk##F)gdpa(dev, "vk" #F)
        O(CreateShaderModule);
        O(CreateSampler);
        O(CreateDescriptorSetLayout);
        O(CreatePipelineLayout);
        O(CreateGraphicsPipelines);
        O(CreateDescriptorPool);
        O(AllocateDescriptorSets);
        O(UpdateDescriptorSets);
        O(CreateImageView);
        O(CmdBeginRendering);
        O(CmdEndRendering);
        O(CmdBindPipeline);
        O(CmdBindDescriptorSets);
        O(CmdSetViewport);
        O(CmdSetScissor);
        O(CmdPushConstants);
        O(CmdDraw);
#undef O
        return true;
    }
};

static VkBoot& GetVkBoot() {
    static VkBoot vb;
    if (!vb.GetInstanceProcAddr) {
        // RTLD_DEFAULT returned NULL — the Vulkan loader isn't in our scope.
        // Open it explicitly. It's already in process memory (Cemu uses it),
        // dlopen just gives us a handle to dlsym against.
        void* vkLib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_GLOBAL);
        if (!vkLib) vkLib = dlopen("libvulkan.so", RTLD_NOW | RTLD_GLOBAL);
        if (!vkLib) {
            std::fprintf(stderr, "[BetterVR-Linux] V2: dlopen libvulkan failed: %s\n", dlerror());
            return vb;
        }
        vb.GetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)
            dlsym(vkLib, "vkGetInstanceProcAddr");
        if (vb.GetInstanceProcAddr) {
            vb.CreateInstance = (PFN_vkCreateInstance)
                vb.GetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
            // GetDeviceProcAddr is instance-level and works with NULL instance.
            vb.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
                vb.GetInstanceProcAddr(VK_NULL_HANDLE, "vkGetDeviceProcAddr");
        }
    }
    return vb;
}

// Drives the OpenXR session through its state machine, submits solid-color
// frames until the runtime tells us to stop. This is the Phase 2B "do you see
// purple in the headset?" milestone.
static void RunFrameLoop(XrInstance xrInstance,
                         XrSystemId xrSystem,
                         XrSession  session,
                         VkDevice   device,
                         const VkBoot& vb)
{
    // ---- 1) Set up Vulkan command recording ------------------------------
    VkDeviceFns dfn;
    if (!dfn.Resolve(device, vb.GetDeviceProcAddr)) {
        std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: failed to resolve device fns\n");
        return;
    }
    VkQueue queue = VK_NULL_HANDLE;
    dfn.GetDeviceQueue(device, 0, 0, &queue);

    // ---- 1.5) Import Cemu's per-eye shared images into the OpenXR-side device.
    // shareds[layer][eye]: layer 0 = 3D scene, layer 1 = 2D HUD.
    SharedImage shareds[2][2];
    {
        std::lock_guard<std::mutex> lk(g_handlesMutex);
        for (int l = 0; l < 2; ++l)
            for (int e = 0; e < 2; ++e)
                shareds[l][e] = g_eyeImages[l][e];
    }
    VkImage importedImages[2][2] = {};
    auto GetMemoryFdPropertiesKHR = (PFN_vkGetMemoryFdPropertiesKHR)
        vb.GetDeviceProcAddr(device, "vkGetMemoryFdPropertiesKHR");

    for (int slot = 0; slot < 4; ++slot) {
        int layer = slot / 2;
        int eye   = slot % 2;
        const SharedImage& shared = shareds[layer][eye];
        if (shared.fd < 0) continue;

        VkExternalMemoryImageCreateInfo extImg = {};
        extImg.sType       = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
        extImg.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkImageCreateInfo ici = {};
        ici.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.pNext         = &extImg;
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = shared.format;
        ici.extent        = { shared.width, shared.height, 1 };
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImage img = VK_NULL_HANDLE;
        if (dfn.CreateImage(device, &ici, nullptr, &img) != VK_SUCCESS) {
            std::fprintf(stderr, "[BetterVR-Linux] FrameLoop[%d]: import vkCreateImage failed\n", eye);
            continue;
        }
        VkMemoryRequirements req = {};
        dfn.GetImageMemoryRequirements(device, img, &req);

        uint32_t typeBits = req.memoryTypeBits;
        if (GetMemoryFdPropertiesKHR) {
            VkMemoryFdPropertiesKHR fdProps = {};
            fdProps.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR;
            if (GetMemoryFdPropertiesKHR(device,
                    VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                    shared.fd, &fdProps) == VK_SUCCESS) {
                typeBits &= fdProps.memoryTypeBits;
            }
        }
        uint32_t typeIdx = UINT32_MAX;
        for (uint32_t i = 0; i < 32; ++i) {
            if (typeBits & (1u << i)) { typeIdx = i; break; }
        }

        VkImportMemoryFdInfoKHR importFd = {};
        importFd.sType      = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR;
        importFd.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        importFd.fd         = shared.fd;

        VkMemoryAllocateInfo mai = {};
        mai.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.pNext           = &importFd;
        mai.allocationSize  = shared.memorySize;
        mai.memoryTypeIndex = typeIdx;

        VkDeviceMemory importedMem = VK_NULL_HANDLE;
        VkResult ar = dfn.AllocateMemory(device, &mai, nullptr, &importedMem);
        std::fprintf(stderr, "[BetterVR-Linux] FrameLoop[%d]: import vkAllocateMemory=%d (typeIdx=%u)\n",
                     eye, (int)ar, typeIdx);
        if (ar == VK_SUCCESS) {
            dfn.BindImageMemory(device, img, importedMem, 0);
            importedImages[layer][eye] = img;
            std::fprintf(stderr, "[BetterVR-Linux] FrameLoop[%d]: imported image bound\n", eye);
        }
    }
    // The existing FrameLoop body uses `importedImage`; keep it pointing at eye 0
    // and override below per XR frame to alternate eyes.
    VkImage importedImage = importedImages[0][0]; // 3D, left eye (legacy alias)
    SharedImage shared = shareds[0][0];

    VkCommandPoolCreateInfo cpci = {};
    cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpci.queueFamilyIndex = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    if (dfn.CreateCommandPool(device, &cpci, nullptr, &pool) != VK_SUCCESS) {
        std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: vkCreateCommandPool failed\n");
        return;
    }

    VkCommandBufferAllocateInfo cbai = {};
    cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbai.commandPool = pool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    dfn.AllocateCommandBuffers(device, &cbai, &cmd);

    // ---- 2) Query view config + create swapchain -------------------------
    uint32_t formatCount = 0;
    xrEnumerateSwapchainFormats(session, 0, &formatCount, nullptr);
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(session, formatCount, &formatCount, formats.data());

    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(xrInstance, xrSystem,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> viewCfgs(viewCount);
    for (auto& v : viewCfgs) v.type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    xrEnumerateViewConfigurationViews(xrInstance, xrSystem,
        XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, viewCount, &viewCount, viewCfgs.data());

    if (formats.empty() || viewCfgs.empty()) return;

    // Mono quad layer: a single 16:9 image projected as a floating screen.
    // Create one per-eye XR swapchain at the headset's recommended per-eye
    // resolution. Each eye gets its own swapchain so we can submit an
    // XrCompositionLayerProjection with per-eye subImages.
    XrSwapchainCreateInfo swInfoTpl = {};
    swInfoTpl.type        = XR_TYPE_SWAPCHAIN_CREATE_INFO;
    swInfoTpl.usageFlags  = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT
                          | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    swInfoTpl.format      = formats[0];
    swInfoTpl.sampleCount = 1;
    swInfoTpl.width       = viewCfgs[0].recommendedImageRectWidth;
    swInfoTpl.height      = viewCfgs[0].recommendedImageRectHeight;
    swInfoTpl.faceCount   = 1;
    swInfoTpl.arraySize   = 1;
    swInfoTpl.mipCount    = 1;

    XrSwapchain swapchains[2] = { XR_NULL_HANDLE, XR_NULL_HANDLE };
    std::vector<XrSwapchainImageVulkanKHR> eyeImages[2];
    // Per-eye, per-image VkImageViews for the dynamic-rendering HUD pass.
    // Pre-created so the per-frame draw can reference them directly.
    std::vector<VkImageView> eyeImageViews[2];
    for (int eye = 0; eye < 2; ++eye) {
        XrSwapchainCreateInfo si = swInfoTpl;
        si.width  = viewCfgs[eye].recommendedImageRectWidth;
        si.height = viewCfgs[eye].recommendedImageRectHeight;
        XrResult r = xrCreateSwapchain(session, &si, &swapchains[eye]);
        if (XR_FAILED(r)) {
            std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: xrCreateSwapchain[%d]=%d\n", eye, (int)r);
            return;
        }
        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(swapchains[eye], 0, &imageCount, nullptr);
        eyeImages[eye].resize(imageCount);
        for (auto& i : eyeImages[eye]) i.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR;
        xrEnumerateSwapchainImages(swapchains[eye], imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(eyeImages[eye].data()));
        eyeImageViews[eye].resize(imageCount, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < imageCount; ++i) {
            VkImageViewCreateInfo ivci = {};
            ivci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image    = eyeImages[eye][i].image;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format   = (VkFormat)formats[0];
            ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            dfn.CreateImageView(device, &ivci, nullptr, &eyeImageViews[eye][i]);
        }
        std::fprintf(stderr,
            "[BetterVR-Linux] FrameLoop: swapchain[%d]=%p %u images %ux%u\n",
            eye, (void*)swapchains[eye], imageCount, si.width, si.height);
    }
    // Keep these aliases so the existing per-XR-frame code (still using
    // `swapchain`, `images`, `swInfo`) builds; we'll reroute for the projection
    // layer just below.
    XrSwapchain swapchain = swapchains[0];
    auto& images = eyeImages[0];
    XrSwapchainCreateInfo swInfo = swInfoTpl;
    swInfo.width  = viewCfgs[0].recommendedImageRectWidth;
    swInfo.height = viewCfgs[0].recommendedImageRectHeight;

    // ---- 3) Reference space (LOCAL — eye-level, fixed at session start) --
    XrReferenceSpaceCreateInfo rsci = {};
    rsci.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace.orientation = { 0, 0, 0, 1 };
    XrSpace localSpace = XR_NULL_HANDLE;
    xrCreateReferenceSpace(session, &rsci, &localSpace);

    // Capture the headset-side aspect ratio (per-eye) — used by the projection
    // hooks when adjusting the game's projection FOV.
    const float headsetAspect = (viewCfgs[0].recommendedImageRectHeight > 0)
        ? (float)viewCfgs[0].recommendedImageRectWidth
          / (float)viewCfgs[0].recommendedImageRectHeight
        : 1.0f;

    // ---- 3.5) HUD overlay pipeline ---------------------------------------
    // Compose BotW's 2D HUD (captured by patch_RND_Find2DFrameBuffer's magic
    // clear → InjectPreClearCapture) on top of the 3D scene by sampling the
    // imported 2D image in a fragment shader that discards pixels matching
    // the magic-clear colors. A plain blit would replace the 3D content with
    // green/blue where the HUD is "empty"; the shader's discard keeps the
    // 3D scene visible through transparent HUD pixels.
    VkShaderModule hudVert = VK_NULL_HANDLE, hudFrag = VK_NULL_HANDLE;
    VkSampler hudSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout hudDSL = VK_NULL_HANDLE;
    VkPipelineLayout hudPL = VK_NULL_HANDLE;
    VkPipeline hudPipeline = VK_NULL_HANDLE;
    VkDescriptorPool hudDP = VK_NULL_HANDLE;
    VkDescriptorSet hudDS[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkImageView hudSrcView[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    bool hudReady = false;
    // BVR_HUD_INIT=0 skips the pipeline build entirely. Used to isolate
    // whether the HUD resource creation (and not the per-frame draw) is
    // affecting overall behavior.
    const bool hudInit = !(std::getenv("BVR_HUD_INIT") && std::getenv("BVR_HUD_INIT")[0] == '0');
    std::fprintf(stderr, "[BetterVR-Linux] HUD init enabled=%d\n", (int)hudInit);
    do {
        if (!hudInit) break;
        VkShaderModuleCreateInfo smci = {};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = sizeof(BetterVR_Linux_Shaders::kHudVertSpv);
        smci.pCode    = BetterVR_Linux_Shaders::kHudVertSpv;
        if (dfn.CreateShaderModule(device, &smci, nullptr, &hudVert) != VK_SUCCESS) break;
        smci.codeSize = sizeof(BetterVR_Linux_Shaders::kHudFragSpv);
        smci.pCode    = BetterVR_Linux_Shaders::kHudFragSpv;
        if (dfn.CreateShaderModule(device, &smci, nullptr, &hudFrag) != VK_SUCCESS) break;

        VkSamplerCreateInfo sci = {};
        sci.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        sci.magFilter    = VK_FILTER_LINEAR;
        sci.minFilter    = VK_FILTER_LINEAR;
        sci.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod       = 0.25f;
        if (dfn.CreateSampler(device, &sci, nullptr, &hudSampler) != VK_SUCCESS) break;

        VkDescriptorSetLayoutBinding dslb = {};
        dslb.binding         = 0;
        dslb.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dslb.descriptorCount = 1;
        dslb.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo dslci = {};
        dslci.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 1;
        dslci.pBindings    = &dslb;
        if (dfn.CreateDescriptorSetLayout(device, &dslci, nullptr, &hudDSL) != VK_SUCCESS) break;

        VkPushConstantRange pcr = {};
        pcr.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pcr.offset     = 0;
        pcr.size       = sizeof(float) * 8; // 2× vec4
        VkPipelineLayoutCreateInfo plci = {};
        plci.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount         = 1;
        plci.pSetLayouts            = &hudDSL;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges    = &pcr;
        if (dfn.CreatePipelineLayout(device, &plci, nullptr, &hudPL) != VK_SUCCESS) break;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = hudVert;
        stages[0].pName  = "main";
        stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = hudFrag;
        stages[1].pName  = "main";

        VkPipelineVertexInputStateCreateInfo vi = {};
        vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo ia = {};
        ia.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo vp = {};
        vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        vp.viewportCount = 1;
        vp.scissorCount  = 1;

        VkPipelineRasterizationStateCreateInfo rs = {};
        rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode    = VK_CULL_MODE_NONE;
        rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth   = 1.0f;

        VkPipelineMultisampleStateCreateInfo ms = {};
        ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState cba = {};
        cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        cba.blendEnable    = VK_FALSE;
        VkPipelineColorBlendStateCreateInfo cb = {};
        cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        cb.attachmentCount = 1;
        cb.pAttachments    = &cba;

        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = {};
        ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        ds.dynamicStateCount = 2;
        ds.pDynamicStates    = dyn;

        VkFormat colorFormat = (VkFormat)formats[0];
        VkPipelineRenderingCreateInfo prci = {};
        prci.sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        prci.colorAttachmentCount    = 1;
        prci.pColorAttachmentFormats = &colorFormat;

        VkGraphicsPipelineCreateInfo gpci = {};
        gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        gpci.pNext               = &prci;
        gpci.stageCount          = 2;
        gpci.pStages             = stages;
        gpci.pVertexInputState   = &vi;
        gpci.pInputAssemblyState = &ia;
        gpci.pViewportState      = &vp;
        gpci.pRasterizationState = &rs;
        gpci.pMultisampleState   = &ms;
        gpci.pColorBlendState    = &cb;
        gpci.pDynamicState       = &ds;
        gpci.layout              = hudPL;
        if (dfn.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, nullptr,
                                        &hudPipeline) != VK_SUCCESS) break;

        VkDescriptorPoolSize dps = {};
        dps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        dps.descriptorCount = 2;
        VkDescriptorPoolCreateInfo dpci = {};
        dpci.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets       = 2;
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes    = &dps;
        if (dfn.CreateDescriptorPool(device, &dpci, nullptr, &hudDP) != VK_SUCCESS) break;

        VkDescriptorSetLayout lyts[2] = { hudDSL, hudDSL };
        VkDescriptorSetAllocateInfo dsai = {};
        dsai.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool     = hudDP;
        dsai.descriptorSetCount = 2;
        dsai.pSetLayouts        = lyts;
        if (dfn.AllocateDescriptorSets(device, &dsai, hudDS) != VK_SUCCESS) break;

        // Build per-eye VkImageViews on the imported 2D images and write
        // the descriptor sets. If an eye's 2D image hasn't been imported
        // (no fd), we skip that eye and the HUD draw will no-op for it.
        for (int eye = 0; eye < 2; ++eye) {
            VkImage src2D = importedImages[1][eye];
            if (src2D == VK_NULL_HANDLE) continue;
            VkImageViewCreateInfo ivci = {};
            ivci.sType    = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image    = src2D;
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format   = shareds[1][eye].format;
            ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            if (dfn.CreateImageView(device, &ivci, nullptr, &hudSrcView[eye]) != VK_SUCCESS) {
                hudSrcView[eye] = VK_NULL_HANDLE;
                continue;
            }
            VkDescriptorImageInfo dii = {};
            dii.sampler     = hudSampler;
            dii.imageView   = hudSrcView[eye];
            // GENERAL layout because the existing 3D blit also samples the
            // imported (OPAQUE_FD-backed) images without explicit transitions
            // and GENERAL is universally compatible. Cross-instance shared
            // memory works in practice on AMD/NVIDIA Linux drivers without
            // strict layout choreography between instances.
            dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet wds = {};
            wds.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wds.dstSet          = hudDS[eye];
            wds.dstBinding      = 0;
            wds.descriptorCount = 1;
            wds.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wds.pImageInfo      = &dii;
            dfn.UpdateDescriptorSets(device, 1, &wds, 0, nullptr);
        }
        hudReady = true;
    } while (false);
    std::fprintf(stderr, "[BetterVR-Linux] HUD pipeline: ready=%d (vert=%p frag=%p pipe=%p eyes=[%p %p])\n",
                 (int)hudReady, (void*)hudVert, (void*)hudFrag, (void*)hudPipeline,
                 (void*)hudSrcView[0], (void*)hudSrcView[1]);

    // ---- 4) Main session loop --------------------------------------------
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested  = false;
    uint64_t frameNo = 0;

    while (!exitRequested) {
        // Drain events
        for (;;) {
            XrEventDataBuffer evt = {};
            evt.type = XR_TYPE_EVENT_DATA_BUFFER;
            if (xrPollEvent(xrInstance, &evt) != XR_SUCCESS) break;
            if (evt.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                auto* sc = reinterpret_cast<XrEventDataSessionStateChanged*>(&evt);
                state = sc->state;
                std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: session state -> %d\n", (int)state);
                if (state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi = {};
                    sbi.type = XR_TYPE_SESSION_BEGIN_INFO;
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    XrResult br = xrBeginSession(session, &sbi);
                    sessionRunning = (br == XR_SUCCESS);
                    std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: xrBeginSession=%d\n", (int)br);
                } else if (state == XR_SESSION_STATE_STOPPING) {
                    xrEndSession(session);
                    sessionRunning = false;
                } else if (state == XR_SESSION_STATE_EXITING ||
                           state == XR_SESSION_STATE_LOSS_PENDING) {
                    exitRequested = true;
                }
            }
        }

        if (!sessionRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // ---- per-frame ----
        XrFrameWaitInfo fwi = {};  fwi.type = XR_TYPE_FRAME_WAIT_INFO;
        XrFrameState   fs  = {};   fs.type  = XR_TYPE_FRAME_STATE;
        xrWaitFrame(session, &fwi, &fs);

        XrFrameBeginInfo fbi = {}; fbi.type = XR_TYPE_FRAME_BEGIN_INFO;
        xrBeginFrame(session, &fbi);

        // Locate the two views and publish their FOVs so the HLE projection
        // hooks (running on Cemu's PPC thread) can rewrite the game's projection
        // matrices per eye. We don't use the returned pose yet — the camera
        // hooks need world-space pose math that's not ported.
        if (fs.shouldRender && localSpace != XR_NULL_HANDLE) {
            XrViewLocateInfo vli = {};
            vli.type                  = XR_TYPE_VIEW_LOCATE_INFO;
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime           = fs.predictedDisplayTime;
            vli.space                 = localSpace;
            XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE;
            uint32_t viewCountOut = 0;
            std::vector<XrView> views(2);
            for (auto& v : views) v.type = XR_TYPE_VIEW;
            XrResult lvr = xrLocateViews(session, &vli, &vs, 2, &viewCountOut, views.data());
            if (lvr == XR_SUCCESS && viewCountOut == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                g_hookState.publishFov(views[0].fov, views[1].fov, headsetAspect);
                g_hookState.publishPose(views[0].pose, views[1].pose);
            }
        }

        // Per-eye XrCompositionLayerProjectionView (built below after we render
        // into each eye's swapchain).
        XrCompositionLayerProjectionView projViews[2] = {};
        XrCompositionLayerProjection     projLayer   = {};
        projLayer.type      = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projLayer.space     = localSpace;
        projLayer.viewCount = 2;
        projLayer.views     = projViews;

        if (fs.shouldRender) {
            // Per-eye: acquire image from eye's swapchain, blit the captured
            // eye image into it, release, fill out the projView.
            //
            // BVR_SWAP_EYES=1 swaps which captured eye image goes to which
            // physical eye, in case BetterVR's eye semantics are reversed
            // relative to OpenXR's view-0/view-1 convention.
            static const bool kSwapEyes = [](){
                const char* v = std::getenv("BVR_SWAP_EYES");
                bool on = v && v[0] && v[0] != '0';
                std::fprintf(stderr, "[BetterVR-Linux] SwapEyes=%d\n", (int)on);
                return on;
            }();
            // Wait for both eyes to be captured since the last consume —
            // matches upstream's Is3DComplete() gate. If only one eye has
            // been refreshed between XR frames, the stereo pair would visibly
            // desync ("one eye janks"). When !bothFresh we skip the swapchain
            // update entirely; the runtime keeps showing the last released
            // image for each eye, and projViews still references those
            // swapchains (and reuses the last consumed pose pair).
            bool bothFresh;
            {
                std::lock_guard<std::mutex> lk(g_capturedPoseMutex);
                bothFresh = g_imageFresh[0] && g_imageFresh[1];
                if (bothFresh) {
                    g_imageFresh[0] = g_imageFresh[1] = false;
                    if (g_capturedPoseValid[0][0]) { g_lastConsumedPose[0] = g_capturedPose[0][0]; g_lastConsumedPoseValid[0] = true; }
                    if (g_capturedPoseValid[0][1]) { g_lastConsumedPose[1] = g_capturedPose[0][1]; g_lastConsumedPoseValid[1] = true; }
                }
            }
            for (int eye = 0; eye < 2; ++eye) {
                int srcEye = kSwapEyes ? (1 - eye) : eye;
                XrSwapchain eyeSwap = swapchains[eye];
                static std::atomic<int> s_perEyeReached{0};
                if (s_perEyeReached.fetch_add(1) < 4) {
                    std::fprintf(stderr, "[BetterVR-Linux] per-eye loop reached eye=%d eyeSwap=%p hudReady=%d hudSrcView[srcEye]=%p\n",
                                 eye, (void*)eyeSwap, (int)hudReady, (void*)hudSrcView[srcEye]);
                }
                if (eyeSwap == XR_NULL_HANDLE) continue;
                uint32_t eyeW = viewCfgs[eye].recommendedImageRectWidth;
                uint32_t eyeH = viewCfgs[eye].recommendedImageRectHeight;
                // Letterbox sizing — computed unconditionally because
                // projViews[eye].subImage.imageRect uses it even when the
                // swapchain isn't updated this frame (bothFresh = false).
                int32_t dstW_full = (int32_t)eyeW;
                int32_t dstH_full = (int32_t)eyeH;
                int32_t srcW = (int32_t)shareds[0][srcEye].width;
                int32_t srcH = (int32_t)shareds[0][srcEye].height;
                int32_t dstW = dstW_full;
                int32_t dstH = dstH_full;
                if (srcW > 0 && srcH > 0) {
                    int32_t fitH = (int32_t)((float)dstW_full * (float)srcH / (float)srcW);
                    if (fitH <= dstH_full) {
                        dstH = fitH;
                    } else {
                        int32_t fitW = (int32_t)((float)dstH_full * (float)srcW / (float)srcH);
                        dstW = fitW;
                    }
                }
                int32_t dstX = (dstW_full - dstW) / 2;
                int32_t dstY = (dstH_full - dstH) / 2;
                {
                uint32_t imgIdx = 0;
                XrSwapchainImageAcquireInfo sai = {}; sai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
                xrAcquireSwapchainImage(eyeSwap, &sai, &imgIdx);
                XrSwapchainImageWaitInfo swi = {}; swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
                swi.timeout = XR_INFINITE_DURATION;
                xrWaitSwapchainImage(eyeSwap, &swi);

                VkImage img = eyeImages[eye][imgIdx].image;
                dfn.ResetCommandBuffer(cmd, 0);
                VkCommandBufferBeginInfo cbbi = {};
                cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                dfn.BeginCommandBuffer(cmd, &cbbi);
                VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                VkImageMemoryBarrier toClear = {};
                toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toClear.image = img;
                toClear.subresourceRange = range;
                dfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                       0, nullptr, 0, nullptr, 1, &toClear);

                VkImage chosen = importedImages[0][srcEye];   // 3D layer
                VkImage chosen2D = importedImages[1][srcEye]; // 2D HUD layer
                if (chosen != VK_NULL_HANDLE) {
                    VkImageBlit blit = {};
                    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    blit.srcOffsets[1]  = { srcW, srcH, 1 };
                    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                    blit.dstOffsets[0]  = { dstX, dstY, 0 };
                    blit.dstOffsets[1]  = { dstX + dstW, dstY + dstH, 1 };
                    dfn.CmdBlitImage(cmd,
                        chosen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        img,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);
                    // 2D over-blit disabled: the 2D buffer is mostly magic-clear
                    // (green/blue) in gameplay because there's no UI most of the
                    // time, so a plain blit would replace the 3D scene with the
                    // magic color. Color-keyed compositing needs a Vulkan
                    // graphics pipeline with a fragment shader that discards
                    // pixels matching the magic clear color. The capture is
                    // still wired (g_eyeImages[1][eye] gets 2D content), ready
                    // for that next step.
                    (void)chosen2D;
                } else {
                    VkClearColorValue color = { eye == 0 ? 0.1f : 0.6f, 0.1f, 0.1f, 1.0f };
                    dfn.CmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           &color, 1, &range);
                }
                VkImageMemoryBarrier toShader = {};
                toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                toShader.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                toShader.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                toShader.image = img;
                toShader.subresourceRange = range;
                dfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                       0, nullptr, 0, nullptr, 1, &toShader);

                // HUD overlay: sample the captured 2D image and discard
                // magic-clear pixels so the 3D scene shows through. The
                // swapchain is now in COLOR_ATTACHMENT_OPTIMAL, ideal for
                // dynamic rendering loadOp=LOAD.
                // BVR_HUD=0 disables the HUD draw entirely (for A/B debugging).
                static const bool kHudEnabled = [](){
                    const char* v = std::getenv("BVR_HUD");
                    bool on = !(v && v[0] == '0');
                    std::fprintf(stderr, "[BetterVR-Linux] HUD draw enabled=%d\n", (int)on);
                    return on;
                }();
                if (kHudEnabled && hudReady && hudSrcView[srcEye] != VK_NULL_HANDLE &&
                    imgIdx < eyeImageViews[eye].size() &&
                    eyeImageViews[eye][imgIdx] != VK_NULL_HANDLE) {
                    VkRenderingAttachmentInfo cai = {};
                    cai.sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
                    cai.imageView   = eyeImageViews[eye][imgIdx];
                    cai.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    cai.loadOp      = VK_ATTACHMENT_LOAD_OP_LOAD;
                    cai.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
                    VkRenderingInfo  ri = {};
                    ri.sType                = VK_STRUCTURE_TYPE_RENDERING_INFO;
                    ri.renderArea           = { { 0, 0 }, { eyeW, eyeH } };
                    ri.layerCount           = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments    = &cai;
                    dfn.CmdBeginRendering(cmd, &ri);

                    VkViewport vpRect = {};
                    // Letterbox region — same rect the 3D blit used so the
                    // HUD aligns with the 3D scene.
                    vpRect.x        = (float)dstX;
                    vpRect.y        = (float)dstY;
                    vpRect.width    = (float)dstW;
                    vpRect.height   = (float)dstH;
                    vpRect.minDepth = 0.0f;
                    vpRect.maxDepth = 1.0f;
                    dfn.CmdSetViewport(cmd, 0, 1, &vpRect);
                    VkRect2D scissor = { { dstX, dstY },
                                         { (uint32_t)dstW, (uint32_t)dstH } };
                    dfn.CmdSetScissor(cmd, 0, 1, &scissor);

                    dfn.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, hudPipeline);
                    dfn.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                              hudPL, 0, 1, &hudDS[srcEye], 0, nullptr);

                    // Push constants: two vec4 (8 floats) — magicA.rgb + tolerance
                    // in .a, magicB.rgb in .rgb (.a unused). The magic 2D
                    // clear values from the BetterVR pack are RGB(0.0625, 0.123,
                    // 0.987) for 2D left and RGB(0.0625, 0.987, 0.123) for
                    // 2D right. We use the SAME pair regardless of eye so
                    // the shader matches whichever side the captured image
                    // came from (covers BVR_SWAP_EYES too).
                    float pc[8] = {
                        0.0625f, 0.123f, 0.987f, 0.06f,   // magicA + tolerance
                        0.0625f, 0.987f, 0.123f, 0.0f,    // magicB
                    };
                    dfn.CmdPushConstants(cmd, hudPL, VK_SHADER_STAGE_FRAGMENT_BIT,
                                         0, sizeof(pc), pc);
                    dfn.CmdDraw(cmd, 3, 1, 0, 0);
                    dfn.CmdEndRendering(cmd);
                }

                dfn.EndCommandBuffer(cmd);
                VkSubmitInfo si = {};
                si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                si.commandBufferCount = 1;
                si.pCommandBuffers    = &cmd;
                dfn.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
                dfn.DeviceWaitIdle(device);

                XrSwapchainImageReleaseInfo sri = {}; sri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
                xrReleaseSwapchainImage(eyeSwap, &sri);
                }

                projViews[eye].type    = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
                projViews[eye].pose    = { {0,0,0,1}, {0,0,0} };
                projViews[eye].fov     = { -1.0f, 1.0f, 1.0f, -1.0f };
                // Use the real pose/fov from xrLocateViews if available.
                if (g_hookState.fovValid[eye]) {
                    std::lock_guard<std::mutex> lk(g_hookState.mtx);
                    projViews[eye].fov = g_hookState.fov[eye];
                }
                projViews[eye].subImage.swapchain       = eyeSwap;
                // Use the content region (after letterboxing) so OpenXR shows
                // only what we actually rendered, not the black borders.
                projViews[eye].subImage.imageRect       = { {dstX, dstY}, {dstW, dstH} };
                projViews[eye].subImage.imageArrayIndex = 0;
            }

            // Re-locate views once more for the up-to-date poses to submit
            // with the projection layer. We use the HEADSET poses but the
            // FOV that BotW actually rendered with (from g_renderedFov) —
            // they may differ from the headset's natural off-axis FOV (e.g.,
            // when BVR_SYMMETRIC_FOV is on).
            XrViewLocateInfo vli = {};
            vli.type                  = XR_TYPE_VIEW_LOCATE_INFO;
            vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
            vli.displayTime           = fs.predictedDisplayTime;
            vli.space                 = localSpace;
            XrViewState vs = {}; vs.type = XR_TYPE_VIEW_STATE;
            uint32_t viewCountOut = 0;
            XrView views2[2] = {};
            views2[0].type = views2[1].type = XR_TYPE_VIEW;
            if (xrLocateViews(session, &vli, &vs, 2, &viewCountOut, views2) == XR_SUCCESS &&
                viewCountOut == 2 &&
                (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
                (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
                projViews[0].pose = views2[0].pose;
                projViews[1].pose = views2[1].pose;
                projViews[0].fov  = views2[0].fov;
                projViews[1].fov  = views2[1].fov;
                std::lock_guard<std::mutex> lk(g_renderedFovMtx);
                int e0 = kSwapEyes ? 1 : 0;
                int e1 = kSwapEyes ? 0 : 1;
                if (g_renderedFovValid[e0]) projViews[0].fov = g_renderedFov[e0];
                if (g_renderedFovValid[e1]) projViews[1].fov = g_renderedFov[e1];
                std::lock_guard<std::mutex> lkPose(g_capturedPoseMutex);
                if (g_capturedPoseValid[0][e0]) projViews[0].pose = g_capturedPose[0][e0];
                if (g_capturedPoseValid[0][e1]) projViews[1].pose = g_capturedPose[0][e1];
            }
        }

        // Unused stub left over from the old quad path — kept to minimize
        // diff churn; the code below the `if (fs.shouldRender)` originally
        // populated quadLayer's subImage and submitted it. We now submit
        // `projLayer` from the per-eye path above.
        if (false) {
            uint32_t imgIdx = 0;
            XrSwapchainImageAcquireInfo sai = {}; sai.type = XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO;
            xrAcquireSwapchainImage(swapchain, &sai, &imgIdx);
            XrSwapchainImageWaitInfo swi = {}; swi.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO;
            swi.timeout = XR_INFINITE_DURATION;
            xrWaitSwapchainImage(swapchain, &swi);

            VkImage img = images[imgIdx].image;
            dfn.ResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo cbbi = {};
            cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            dfn.BeginCommandBuffer(cmd, &cbbi);

            VkImageSubresourceRange range = {};
            range.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel   = 0;
            range.levelCount     = 1;
            range.baseArrayLayer = 0;
            range.layerCount     = 1; // mono quad layer

            VkImageMemoryBarrier toClear = {};
            toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toClear.srcAccessMask       = 0;
            toClear.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            toClear.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
            toClear.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toClear.image               = img;
            toClear.subresourceRange    = range;
            dfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                   0, nullptr, 0, nullptr, 1, &toClear);

            // BVR_FORCE_HUE=1 forces the animated-hue fallback path, ignoring
            // the imported Cemu image. Lets us isolate "is the XR layer pipeline
            // healthy?" from "is the cross-instance image sharing healthy?".
            static const bool kForceHue = [](){
                const char* v = std::getenv("BVR_FORCE_HUE");
                return v && v[0] && v[0] != '0';
            }();

            // Alternate which eye image we display each XR frame. If per-eye
            // capture works, the user will SEE the quad flicker between two
            // visibly different views (or, if BVR_FORCE_EYE is set, that eye
            // is shown consistently for a stable test).
            static const int kForceEye = [](){
                const char* v = std::getenv("BVR_FORCE_EYE");
                if (!v) return -1;
                if (v[0] == '0') return 0;
                if (v[0] == '1') return 1;
                return -1;
            }();
            int displayEye = (kForceEye >= 0) ? kForceEye : (int)(frameNo & 1);
            VkImage chosen = importedImages[0][displayEye]; // legacy path: 3D layer
            const SharedImage& chosenShared = shareds[0][displayEye];

            if (chosen != VK_NULL_HANDLE && !kForceHue) {
                // Blit the imported (Cemu-side) image to the swapchain image.
                VkImageBlit blit = {};
                blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.srcOffsets[1]  = { (int32_t)chosenShared.width, (int32_t)chosenShared.height, 1 };
                blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
                blit.dstOffsets[1]  = { (int32_t)swInfo.width, (int32_t)swInfo.height, 1 };
                dfn.CmdBlitImage(cmd,
                    chosen, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    img,    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1, &blit, VK_FILTER_LINEAR);
            } else {
                // Fallback: animated hue (no shared image available, or forced
                // by BVR_FORCE_HUE).
                float t = (float)(frameNo % 240) / 240.0f;
                VkClearColorValue color = {};
                color.float32[0] = 0.5f + 0.5f * std::sin(t * 6.2832f);
                color.float32[1] = 0.5f + 0.5f * std::sin(t * 6.2832f + 2.094f);
                color.float32[2] = 0.5f + 0.5f * std::sin(t * 6.2832f + 4.188f);
                color.float32[3] = 1.0f;
                dfn.CmdClearColorImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       &color, 1, &range);
            }

            VkImageMemoryBarrier toShader = {};
            toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            toShader.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
            toShader.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            toShader.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            toShader.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            toShader.image               = img;
            toShader.subresourceRange    = range;
            dfn.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                   0, nullptr, 0, nullptr, 1, &toShader);
            dfn.EndCommandBuffer(cmd);

            VkSubmitInfo si = {};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.commandBufferCount = 1;
            si.pCommandBuffers    = &cmd;
            dfn.QueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
            dfn.DeviceWaitIdle(device); // simplest possible sync

            XrSwapchainImageReleaseInfo sri = {}; sri.type = XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO;
            xrReleaseSwapchainImage(swapchain, &sri);
        }

        XrFrameEndInfo fei = {};
        fei.type                 = XR_TYPE_FRAME_END_INFO;
        fei.displayTime          = fs.predictedDisplayTime;
        fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        const XrCompositionLayerBaseHeader* layers[] = {
            reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projLayer)
        };
        if (fs.shouldRender) {
            fei.layerCount = 1;
            fei.layers     = layers;
        }
        xrEndFrame(session, &fei);
        ++frameNo;
        if ((frameNo % 90) == 0) {
            std::fprintf(stderr,
                "[BetterVR-Linux] FrameLoop: %lu | BeginRP=%lu EndRend=%lu SwapHits=%lu Inj=%lu (L=%lu R=%lu) | EyeImg(L=%lu R=%lu) | BeginCamera=%lu GetRenderProj=%lu LightPrePass=%lu | currentEye=%d swapIdx=%u\n",
                frameNo,
                g_beginRenderPassCount.load(std::memory_order_relaxed),
                g_endRenderingCount.load(std::memory_order_relaxed),
                g_endRenderingSwapchainHits.load(std::memory_order_relaxed),
                g_injectedCopies.load(std::memory_order_relaxed),
                g_injectedCopiesPerEye[0].load(std::memory_order_relaxed),
                g_injectedCopiesPerEye[1].load(std::memory_order_relaxed),
                g_imageEyeIdentified[0].load(std::memory_order_relaxed),
                g_imageEyeIdentified[1].load(std::memory_order_relaxed),
                g_hookState.hitsBeginCameraSide.load(std::memory_order_relaxed),
                g_hookState.hitsGetRenderProjection.load(std::memory_order_relaxed),
                g_hookState.hitsLightPrePass.load(std::memory_order_relaxed),
                g_currentEye.load(std::memory_order_relaxed),
                g_activeSwapImageIndex.load(std::memory_order_relaxed));
        }
    }

    if (localSpace != XR_NULL_HANDLE) xrDestroySpace(localSpace);
    xrDestroySwapchain(swapchain);
    std::fprintf(stderr, "[BetterVR-Linux] FrameLoop: exiting after %lu frames\n", frameNo);
}

static void TryCreateOpenXRSession() {
    auto& vb = GetVkBoot();
    if (!vb.GetInstanceProcAddr) {
        std::fprintf(stderr, "[BetterVR-Linux] V3: VulkanBoot failed: gipa=%p\n",
                     (void*)vb.GetInstanceProcAddr);
        return;
    }

    // ---- 1) Create OpenXR instance with VULKAN_ENABLE2 --------------------
    // The key insight from native-test bisection: WiVRn's V1 entry points
    // crash because vk_get_instance_proc_addr is unset on its system state.
    // V2 chain takes pfnGetInstanceProcAddr at xrCreateVulkanInstanceKHR time
    // and populates it, so subsequent calls (Device, CreateSession) succeed.
    const char* xrExts[] = { XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME };
    XrInstanceCreateInfo xrInfo = {};
    xrInfo.type = XR_TYPE_INSTANCE_CREATE_INFO;
    xrInfo.enabledExtensionCount = 1;
    xrInfo.enabledExtensionNames = xrExts;
    xrInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    std::snprintf(xrInfo.applicationInfo.applicationName,
                  sizeof(xrInfo.applicationInfo.applicationName),
                  "BetterVR-Linux V3 Probe");
    xrInfo.applicationInfo.applicationVersion = 1;
    std::snprintf(xrInfo.applicationInfo.engineName,
                  sizeof(xrInfo.applicationInfo.engineName), "BetterVR");
    xrInfo.applicationInfo.engineVersion = 1;

    XrInstance xrInstance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&xrInfo, &xrInstance);
    std::fprintf(stderr, "[BetterVR-Linux] V3: xrCreateInstance=%d\n", (int)r);
    if (XR_FAILED(r)) return;

    XrSystemGetInfo sysGetInfo = {};
    sysGetInfo.type = XR_TYPE_SYSTEM_GET_INFO;
    sysGetInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId xrSystem = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(xrInstance, &sysGetInfo, &xrSystem);
    std::fprintf(stderr, "[BetterVR-Linux] V3: xrGetSystem=%d system=0x%llx\n",
                 (int)r, (unsigned long long)xrSystem);
    if (XR_FAILED(r)) { xrDestroyInstance(xrInstance); return; }

    PFN_xrGetVulkanGraphicsRequirements2KHR pfnReqs2 = nullptr;
    xrGetInstanceProcAddr(xrInstance, "xrGetVulkanGraphicsRequirements2KHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnReqs2));
    if (pfnReqs2) {
        XrGraphicsRequirementsVulkan2KHR reqs2 = {};
        reqs2.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR;
        r = pfnReqs2(xrInstance, xrSystem, &reqs2);
        std::fprintf(stderr, "[BetterVR-Linux] V3: GraphicsRequirements2=%d\n", (int)r);
    }

    // ---- 2) Have OpenXR create our VkInstance — this teaches it our loader -
    PFN_xrCreateVulkanInstanceKHR pfnCreateVkInst = nullptr;
    xrGetInstanceProcAddr(xrInstance, "xrCreateVulkanInstanceKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVkInst));
    std::fprintf(stderr, "[BetterVR-Linux] V3: pfnCreateVkInst=%p\n", (void*)pfnCreateVkInst);
    if (!pfnCreateVkInst) { xrDestroyInstance(xrInstance); return; }

    VkApplicationInfo appInfo = {};
    appInfo.sType            = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "BetterVR-Linux OpenXR side";
    appInfo.applicationVersion = 1;
    appInfo.pEngineName      = "BetterVR";
    appInfo.engineVersion    = 1;
    appInfo.apiVersion       = VK_API_VERSION_1_3;

    VkInstanceCreateInfo vkici = {};
    vkici.sType            = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    vkici.pApplicationInfo = &appInfo;

    XrVulkanInstanceCreateInfoKHR xvki = {};
    xvki.type                   = XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR;
    xvki.systemId               = xrSystem;
    xvki.pfnGetInstanceProcAddr = vb.GetInstanceProcAddr;
    xvki.vulkanCreateInfo       = &vkici;

    VkInstance ourVkInstance = VK_NULL_HANDLE;
    VkResult vkResult = VK_SUCCESS;
    r = pfnCreateVkInst(xrInstance, &xvki, &ourVkInstance, &vkResult);
    std::fprintf(stderr, "[BetterVR-Linux] V3: xrCreateVulkanInstanceKHR xr=%d vk=%d instance=%p\n",
                 (int)r, (int)vkResult, (void*)ourVkInstance);
    if (XR_FAILED(r) || vkResult != VK_SUCCESS) { xrDestroyInstance(xrInstance); return; }

    // Now that we have a real instance, re-resolve GetDeviceProcAddr.
    // (With VK_NULL_HANDLE it returns NULL — that's spec-correct.)
    vb.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)
        vb.GetInstanceProcAddr(ourVkInstance, "vkGetDeviceProcAddr");
    std::fprintf(stderr, "[BetterVR-Linux] V3: GetDeviceProcAddr=%p (resolved via ourVkInstance)\n",
                 (void*)vb.GetDeviceProcAddr);

    // ---- 3) Now query the matching physical device ------------------------
    PFN_xrGetVulkanGraphicsDevice2KHR pfnGetGfxDev2 = nullptr;
    xrGetInstanceProcAddr(xrInstance, "xrGetVulkanGraphicsDevice2KHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnGetGfxDev2));

    VkPhysicalDevice xrPhysDev = VK_NULL_HANDLE;
    if (pfnGetGfxDev2) {
        XrVulkanGraphicsDeviceGetInfoKHR gdi = {};
        gdi.type            = XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR;
        gdi.systemId        = xrSystem;
        gdi.vulkanInstance  = ourVkInstance;
        r = pfnGetGfxDev2(xrInstance, &gdi, &xrPhysDev);
        std::fprintf(stderr, "[BetterVR-Linux] V3: xrGetVulkanGraphicsDevice2KHR=%d xrPhysDev=%p\n",
                     (int)r, (void*)xrPhysDev);
    }

    // ---- 4) Let OpenXR create the matching VkDevice ----------------------
    PFN_xrCreateVulkanDeviceKHR pfnCreateVkDev = nullptr;
    xrGetInstanceProcAddr(xrInstance, "xrCreateVulkanDeviceKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&pfnCreateVkDev));

    VkDevice ourVkDevice = VK_NULL_HANDLE;
    if (pfnCreateVkDev && xrPhysDev) {
        float queuePriority = 1.0f;
        VkDeviceQueueCreateInfo qci = {};
        qci.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = 0;  // assume family 0 for graphics; TODO: query properly
        qci.queueCount       = 1;
        qci.pQueuePriorities = &queuePriority;

        // Enable dynamicRendering (Vulkan 1.3 core feature) so the HUD
        // overlay pass can draw to the swapchain image without managing
        // VkRenderPass/VkFramebuffer objects. Chain the features struct
        // into VkDeviceCreateInfo's pNext.
        VkPhysicalDeviceVulkan13Features vk13Features = {};
        vk13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
        vk13Features.dynamicRendering = VK_TRUE;

        VkDeviceCreateInfo dci = {};
        dci.sType                 = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.pNext                 = &vk13Features;
        dci.queueCreateInfoCount  = 1;
        dci.pQueueCreateInfos     = &qci;

        XrVulkanDeviceCreateInfoKHR xvdi = {};
        xvdi.type                   = XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR;
        xvdi.systemId               = xrSystem;
        xvdi.pfnGetInstanceProcAddr = vb.GetInstanceProcAddr;
        xvdi.vulkanPhysicalDevice   = xrPhysDev;
        xvdi.vulkanCreateInfo       = &dci;

        r = pfnCreateVkDev(xrInstance, &xvdi, &ourVkDevice, &vkResult);
        std::fprintf(stderr, "[BetterVR-Linux] V3: xrCreateVulkanDeviceKHR xr=%d vk=%d device=%p\n",
                     (int)r, (int)vkResult, (void*)ourVkDevice);
    }

    // ---- 5) Try the session create on the V2 binding ----------------------
    if (ourVkDevice) {
        XrGraphicsBindingVulkan2KHR vkBinding = {};
        vkBinding.type             = XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR;
        vkBinding.instance         = ourVkInstance;
        vkBinding.physicalDevice   = xrPhysDev;
        vkBinding.device           = ourVkDevice;
        vkBinding.queueFamilyIndex = 0;
        vkBinding.queueIndex       = 0;

        XrSessionCreateInfo sci = {};
        sci.type     = XR_TYPE_SESSION_CREATE_INFO;
        sci.next     = &vkBinding;
        sci.systemId = xrSystem;

        XrSession session = XR_NULL_HANDLE;
        r = xrCreateSession(xrInstance, &sci, &session);
        std::fprintf(stderr, "[BetterVR-Linux] V3: xrCreateSession=%d session=%p\n",
                     (int)r, (void*)session);

        if (session != XR_NULL_HANDLE) {
            RunFrameLoop(xrInstance, xrSystem, session, ourVkDevice, vb);
            xrDestroySession(session);
        }
    }

    // (Skip device/instance destroy — OpenXR-created handles need
    //  matching destroy paths; cleanup left for the real implementation.)
    xrDestroyInstance(xrInstance);
    std::fprintf(stderr, "[BetterVR-Linux] V3: probe complete\n");
}

class VkInstanceOverrides {
public:
    static VkResult CreateInstance(
        PFN_vkCreateInstance      pfnCreateInstance,
        const VkInstanceCreateInfo* createInfo,
        const VkAllocationCallbacks* allocator,
        VkInstance*                instance)
    {
        std::fprintf(stderr, "[BetterVR-Linux] vkCreateInstance hook\n");
        ProbeCemuExports();

        VkResult r = pfnCreateInstance(createInfo, allocator, instance);
        if (r == VK_SUCCESS && instance && *instance) {
            std::lock_guard<std::mutex> lock(g_handlesMutex);
            g_handles.instance   = *instance;
            g_handles.apiVersion = createInfo->pApplicationInfo
                                     ? createInfo->pApplicationInfo->apiVersion
                                     : 0;
        }
        return r;
    }

    static VkResult CreateDevice(
        const vkroots::VkPhysicalDeviceDispatch& pDispatch,
        VkPhysicalDevice           physicalDevice,
        const VkDeviceCreateInfo*  createInfo,
        const VkAllocationCallbacks* allocator,
        VkDevice*                  device)
    {
        std::fprintf(stderr, "[BetterVR-Linux] vkCreateDevice hook (physDev=%p)\n",
                     (void*)physicalDevice);

        // We need VK_KHR_external_memory_fd on Cemu's device so we can export
        // memory across to the OpenXR-side device. Inject it into Cemu's
        // requested extension list if not already there. (vkroots gives us a
        // pCreateInfo that's safe to copy; the loader doesn't keep the pointer
        // after this call returns.)
        std::vector<const char*> extensions;
        bool isFirstDevice;
        {
            std::lock_guard<std::mutex> lock(g_handlesMutex);
            isFirstDevice = (g_handles.device == VK_NULL_HANDLE);
        }

        VkDeviceCreateInfo patched = *createInfo;
        if (isFirstDevice) {
            extensions.assign(createInfo->ppEnabledExtensionNames,
                              createInfo->ppEnabledExtensionNames + createInfo->enabledExtensionCount);
            auto need = {
                VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            };
            for (const char* e : need) {
                bool found = false;
                for (auto* ee : extensions) if (!std::strcmp(ee, e)) { found = true; break; }
                if (!found) extensions.push_back(e);
            }
            patched.enabledExtensionCount   = (uint32_t)extensions.size();
            patched.ppEnabledExtensionNames = extensions.data();
        }

        VkResult r = pDispatch.CreateDevice(physicalDevice, &patched, allocator, device);
        if (r == VK_SUCCESS && device && *device) {
            uint32_t qfam = UINT32_MAX;
            for (uint32_t i = 0; i < createInfo->queueCreateInfoCount; ++i) {
                const auto& qci = createInfo->pQueueCreateInfos[i];
                if (qci.queueCount > 0) { qfam = qci.queueFamilyIndex; break; }
            }

            bool shouldSetupShared = false;
            VkInstance inst{}; VkDevice dev{};
            {
                std::lock_guard<std::mutex> lock(g_handlesMutex);
                if (g_handles.device == VK_NULL_HANDLE) {
                    g_handles.physicalDevice      = physicalDevice;
                    g_handles.device              = *device;
                    g_handles.graphicsQueueFamily = qfam;
                    g_handles.graphicsQueueIndex  = 0;
                    inst = g_handles.instance;
                    dev  = *device;
                    shouldSetupShared = true;
                }
            }
            (void)shouldSetupShared; (void)inst; (void)dev;
            // SetupSharedImageFromCemuDevice deferred to the QueuePresentKHR
            // worker thread — vkroots populates DeviceDispatches *after* our
            // CreateDevice override returns.
        }
        return r;
    }
};

class VkDeviceOverrides {
public:
    static VkResult CreateSwapchainKHR(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                          device,
        const VkSwapchainCreateInfoKHR*  pCreateInfo,
        const VkAllocationCallbacks*      pAllocator,
        VkSwapchainKHR*                   pSwapchain)
    {
        VkResult r = pDispatch.CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
        if (r == VK_SUCCESS && pSwapchain && *pSwapchain) {
            std::fprintf(stderr,
                "[BetterVR-Linux] vkCreateSwapchainKHR: %ux%u format=%d imageCount(min)=%u handle=%p\n",
                pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
                (int)pCreateInfo->imageFormat, pCreateInfo->minImageCount, (void*)*pSwapchain);

            uint32_t n = 0;
            pDispatch.GetSwapchainImagesKHR(device, *pSwapchain, &n, nullptr);
            std::vector<VkImage> imgs(n);
            pDispatch.GetSwapchainImagesKHR(device, *pSwapchain, &n, imgs.data());

            std::lock_guard<std::mutex> lock(g_handlesMutex);
            g_cemuSwap.handle = *pSwapchain;
            g_cemuSwap.width  = pCreateInfo->imageExtent.width;
            g_cemuSwap.height = pCreateInfo->imageExtent.height;
            g_cemuSwap.format = pCreateInfo->imageFormat;
            g_cemuSwap.images = std::move(imgs);
        }
        return r;
    }

    static VkResult QueuePresentKHR(
        const vkroots::VkQueueDispatch& pDispatch,
        VkQueue                         queue,
        const VkPresentInfoKHR*         presentInfo)
    {
        bool expected = false;
        if (g_xrSessionAttempted.compare_exchange_strong(expected, true)) {
            std::thread([]{
                CapturedHandles h;
                { std::lock_guard<std::mutex> lock(g_handlesMutex); h = g_handles; }
                if (h.device != VK_NULL_HANDLE) {
                    SetupSharedImageFromCemuDevice(h.instance, h.physicalDevice,
                        h.device, h.graphicsQueueFamily, GetVkBoot());
                }
                TryCreateOpenXRSession();
            }).detach();
        }

        // First time we see a given Cemu swapchain handle in QueuePresentKHR,
        // log it. Helps debug "user sees green / wrong frame" — it tells us
        // which swapchain Cemu is actually presenting to (BotW main render
        // vs Cemu GUI/pad).
        if (presentInfo && presentInfo->swapchainCount > 0) {
            VkSwapchainKHR sc = presentInfo->pSwapchains[0];
            static std::mutex seenMtx;
            static std::vector<VkSwapchainKHR> seen;
            bool isNew = false;
            {
                std::lock_guard<std::mutex> lk(seenMtx);
                if (std::find(seen.begin(), seen.end(), sc) == seen.end()) {
                    seen.push_back(sc);
                    isNew = true;
                }
            }
            if (isNew) {
                VkSwapchainKHR tracked = VK_NULL_HANDLE;
                { std::lock_guard<std::mutex> lk(g_handlesMutex); tracked = g_cemuSwap.handle; }
                std::fprintf(stderr,
                    "[BetterVR-Linux] QueuePresentKHR: new swapchain present sc=%p (g_cemuSwap.handle=%p match=%d)\n",
                    (void*)sc, (void*)tracked, sc == tracked ? 1 : 0);
            }
        }

        // If our shared image is set up and Cemu is presenting one of our
        // captured swapchain images, blit it into the shared image so the
        // OpenXR side picks it up next frame.
        CopyPresentToSharedIfReady(presentInfo);

        return pDispatch.QueuePresentKHR(queue, presentInfo);
    }

    // Track VkImage → (width, height) so InjectPreClearCapture can blit using
    // the actual image extent (not Cemu's swapchain extent, which is wrong for
    // post-HDR composed images that BotW renders at internal resolution).
    static VkResult CreateImage(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        const VkImageCreateInfo*         pCreateInfo,
        const VkAllocationCallbacks*     pAllocator,
        VkImage*                         pImage)
    {
        VkResult r = pDispatch.CreateImage(device, pCreateInfo, pAllocator, pImage);
        if (r == VK_SUCCESS && pImage && *pImage && pCreateInfo) {
            std::lock_guard<std::mutex> lk(g_imageExtentMutex);
            g_imageExtent[*pImage] = { pCreateInfo->extent.width, pCreateInfo->extent.height };
        }
        return r;
    }
    static void DestroyImage(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        VkImage                          image,
        const VkAllocationCallbacks*     pAllocator)
    {
        if (image != VK_NULL_HANDLE) {
            { std::lock_guard<std::mutex> lk(g_imageExtentMutex); g_imageExtent.erase(image); }
            { std::lock_guard<std::mutex> lk(g_imageEyeMutex);    g_imageEye.erase(image); }
        }
        pDispatch.DestroyImage(device, image, pAllocator);
    }

    // Track VkImageView → VkImage so we can later check if a render pass's
    // color attachment views point at swapchain images.
    static VkResult CreateImageView(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        const VkImageViewCreateInfo*     pCreateInfo,
        const VkAllocationCallbacks*     pAllocator,
        VkImageView*                     pView)
    {
        VkResult r = pDispatch.CreateImageView(device, pCreateInfo, pAllocator, pView);
        if (r == VK_SUCCESS && pView && *pView && pCreateInfo) {
            std::lock_guard<std::mutex> lk(g_viewToImageMutex);
            g_viewToImage[*pView] = pCreateInfo->image;
        }
        return r;
    }
    static void DestroyImageView(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        VkImageView                      view,
        const VkAllocationCallbacks*     pAllocator)
    {
        if (view != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> lk(g_viewToImageMutex);
            g_viewToImage.erase(view);
        }
        pDispatch.DestroyImageView(device, view, pAllocator);
    }

    // Magic-color detector. BetterVR's Find{2D,3D}FrameBuffer patches clear
    // BotW's post-HDR composed image to fixed float patterns. Cemu round-trips
    // through BGRA8 quantization (e.g., 0.0625 → 15/255 = 0.0588, 0.123 →
    // 31/255 = 0.1216, 0.987 → 251/255 = 0.9843), so we use a wide tolerance.
    //   3D left  ≈ (0.0,    0.1216, 0.9843)   — the actual 3D scene (what we want)
    //   3D right ≈ (0.0,    0.9843, 0.1216)
    //   2D left  ≈ (0.0588, 0.1216, 0.9843)   — UI/HUD overlay layer
    //   2D right ≈ (0.0588, 0.9843, 0.1216)
    // Returns the eye index encoded with a +10 offset for 3D so callers can
    // tell 2D vs 3D apart: -1 = no match, 0/1 = 2D L/R, 10/11 = 3D L/R.
    // Plain "eye" can be recovered as result % 10.
    static int ClassifyMagicColor(const VkClearColorValue& c) {
        auto near_eq = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
        const bool r0      = near_eq(c.float32[0], 0.0f);
        const bool r0_0625 = near_eq(c.float32[0], 0.0625f) || near_eq(c.float32[0], 0.0588f);
        const bool g_lo    = near_eq(c.float32[1], 0.123f) || near_eq(c.float32[1], 0.1216f);
        const bool g_hi    = near_eq(c.float32[1], 0.987f) || near_eq(c.float32[1], 0.9843f);
        const bool b_lo    = near_eq(c.float32[2], 0.123f) || near_eq(c.float32[2], 0.1216f);
        const bool b_hi    = near_eq(c.float32[2], 0.987f) || near_eq(c.float32[2], 0.9843f);
        if (r0 && g_lo && b_hi) return 10;        // 3D left
        if (r0 && g_hi && b_lo) return 11;        // 3D right
        if (r0_0625 && g_lo && b_hi) return 0;    // 2D left
        if (r0_0625 && g_hi && b_lo) return 1;    // 2D right
        return -1;
    }

    // Intercept Cemu's translation of GX2ClearBuffersEx. If the color pattern
    // matches a BetterVR magic, record image → eye in g_imageEye.
    static void CmdClearColorImage(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        VkImage                                 image,
        VkImageLayout                           imageLayout,
        const VkClearColorValue*                pColor,
        uint32_t                                rangeCount,
        const VkImageSubresourceRange*          pRanges)
    {
        // Trace first 30 NON-BLACK clears so we can see what colors Cemu emits
        // when it does meaningful clears (skip the boring (0,0,0,0) init ones).
        if (pColor) {
            auto& c = *pColor;
            bool nonBlack = std::fabs(c.float32[0]) > 0.001f ||
                            std::fabs(c.float32[1]) > 0.001f ||
                            std::fabs(c.float32[2]) > 0.001f;
            static std::atomic<int> traceN{0};
            if (nonBlack && traceN.fetch_add(1, std::memory_order_relaxed) < 30) {
                std::fprintf(stderr,
                    "[BetterVR-Linux] CmdClearColorImage: img=%p color=(%.4f, %.4f, %.4f, %.4f)\n",
                    (void*)image,
                    (double)c.float32[0], (double)c.float32[1],
                    (double)c.float32[2], (double)c.float32[3]);
            }
        }
        int raw = pColor ? ClassifyMagicColor(*pColor) : -1;
        // 0/1 = 2D HUD; 10/11 = 3D scene. We capture both into separate eye
        // images (g_eyeImages[0][eye] = 3D, g_eyeImages[1][eye] = 2D).
        int magicLayer = (raw >= 10) ? 0 : (raw >= 0 ? 1 : -1);
        int magicEye   = (raw >= 0) ? (raw % 10) : -1;
        if (magicEye >= 0 && image != VK_NULL_HANDLE) {
            bool newly = false;
            {
                std::lock_guard<std::mutex> lk(g_imageEyeMutex);
                auto it = g_imageEye.find(image);
                if (it == g_imageEye.end()) {
                    g_imageEye[image] = magicEye;
                    newly = true;
                } else {
                    it->second = magicEye;
                }
            }
            if (newly) {
                g_imageEyeIdentified[magicEye].fetch_add(1, std::memory_order_relaxed);
                ImageExtent2D ext = {};
                {
                    std::lock_guard<std::mutex> lk(g_imageExtentMutex);
                    auto it = g_imageExtent.find(image);
                    if (it != g_imageExtent.end()) ext = it->second;
                }
                std::fprintf(stderr,
                    "[BetterVR-Linux] Magic clear (first seen): eye=%d image=%p extent=%ux%u\n",
                    magicEye, (void*)image, ext.w, ext.h);
            }
            // Filters tried and reverted: a "dirty-since-last-capture" gate
            // and a "captured-this-frame" gate both starved the capture of
            // legitimate 3D-buffer content. Capturing on every magic clear is
            // wasteful but empirically gives the user the freshest content.
            InjectPreClearCapture(pDispatch, commandBuffer, image, imageLayout, magicLayer, magicEye);
        }
        pDispatch.CmdClearColorImage(commandBuffer, image, imageLayout,
                                     pColor, rangeCount, pRanges);
    }

    // vkCmdClearAttachments operates inside a render pass on the currently-bound
    // framebuffer's attachments. We don't know which attachment index maps to
    // which image without tracking the active framebuffer per cmdbuf. For now,
    // if any clear color matches a magic, and the cmdbuf is inside a swap-
    // adjacent render pass, mark the cmdbuf with the corresponding eye — this
    // is the simplest way to attribute the eye if BotW uses ClearAttachments
    // instead of ClearColorImage. (Color attachments only.)
    static void CmdClearAttachments(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        uint32_t                                attachmentCount,
        const VkClearAttachment*                pAttachments,
        uint32_t                                rectCount,
        const VkClearRect*                      pRects)
    {
        if (pAttachments) {
            for (uint32_t i = 0; i < attachmentCount; ++i) {
                if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                    const auto& c = pAttachments[i].clearValue.color;
                    bool nonBlack = std::fabs(c.float32[0]) > 0.001f ||
                                    std::fabs(c.float32[1]) > 0.001f ||
                                    std::fabs(c.float32[2]) > 0.001f;
                    static std::atomic<int> traceN{0};
                    if (nonBlack && traceN.fetch_add(1, std::memory_order_relaxed) < 30) {
                        std::fprintf(stderr,
                            "[BetterVR-Linux] CmdClearAttachments[%u]: color=(%.4f, %.4f, %.4f, %.4f)\n",
                            i,
                            (double)c.float32[0], (double)c.float32[1],
                            (double)c.float32[2], (double)c.float32[3]);
                    }
                }
            }
        }
        if (pAttachments) {
            for (uint32_t i = 0; i < attachmentCount; ++i) {
                if (pAttachments[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
                    int raw = ClassifyMagicColor(pAttachments[i].clearValue.color);
                    if (raw >= 0) {
                        int eye = raw % 10;
                        std::lock_guard<std::mutex> lk(g_cmdbufEyeMutex);
                        g_cmdbufEye[commandBuffer] = eye;
                        break;
                    }
                }
            }
        }
        pDispatch.CmdClearAttachments(commandBuffer, attachmentCount, pAttachments,
                                      rectCount, pRects);
    }

    // Track VkFramebuffer → attachment VkImages. Cemu's legacy render-pass code
    // path passes a VkFramebuffer to vkCmdBeginRenderPass; we need to know
    // whether any of its color attachments is a swapchain image.
    static VkResult CreateFramebuffer(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        const VkFramebufferCreateInfo*   pCreateInfo,
        const VkAllocationCallbacks*     pAllocator,
        VkFramebuffer*                   pFramebuffer)
    {
        VkResult r = pDispatch.CreateFramebuffer(device, pCreateInfo, pAllocator, pFramebuffer);
        if (r == VK_SUCCESS && pFramebuffer && *pFramebuffer && pCreateInfo) {
            std::vector<VkImage> imgs;
            imgs.reserve(pCreateInfo->attachmentCount);
            {
                std::lock_guard<std::mutex> lk(g_viewToImageMutex);
                for (uint32_t i = 0; i < pCreateInfo->attachmentCount; ++i) {
                    VkImageView v = pCreateInfo->pAttachments[i];
                    auto it = g_viewToImage.find(v);
                    if (it != g_viewToImage.end()) imgs.push_back(it->second);
                }
            }
            std::lock_guard<std::mutex> lk(g_framebufferImagesMutex);
            g_framebufferImages[*pFramebuffer] = std::move(imgs);
        }
        return r;
    }
    static void DestroyFramebuffer(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        VkFramebuffer                    framebuffer,
        const VkAllocationCallbacks*     pAllocator)
    {
        if (framebuffer != VK_NULL_HANDLE) {
            std::lock_guard<std::mutex> lk(g_framebufferImagesMutex);
            g_framebufferImages.erase(framebuffer);
        }
        pDispatch.DestroyFramebuffer(device, framebuffer, pAllocator);
    }

    // Legacy render-pass begin variants. Cemu may use these instead of (or in
    // addition to) dynamic rendering. The pClearValues in pRenderPassBegin
    // is the most common Vulkan path for translating GX2ClearBuffersEx.
    static void HandleBeginRenderPassClearValues(VkCommandBuffer cb,
                                                 const VkRenderPassBeginInfo* info) {
        if (!info || info->clearValueCount == 0 || !info->pClearValues) return;
        // Trace first 30 non-black colored BeginRP clears so we catch the
        // patches' actual magic colors instead of boring (0,0,0,0) init clears.
        for (uint32_t i = 0; i < info->clearValueCount; ++i) {
            const auto& c = info->pClearValues[i].color;
            bool nonBlack = std::fabs(c.float32[0]) > 0.001f ||
                            std::fabs(c.float32[1]) > 0.001f ||
                            std::fabs(c.float32[2]) > 0.001f;
            static std::atomic<int> traceN{0};
            if (nonBlack && traceN.fetch_add(1, std::memory_order_relaxed) < 30) {
                std::fprintf(stderr,
                    "[BetterVR-Linux] BeginRP cv[%u]: color=(%.4f, %.4f, %.4f, %.4f)\n",
                    i,
                    (double)c.float32[0], (double)c.float32[1],
                    (double)c.float32[2], (double)c.float32[3]);
            }
        }
        if (info->framebuffer == VK_NULL_HANDLE) return;
        std::vector<VkImage> fbImages;
        {
            std::lock_guard<std::mutex> lk(g_framebufferImagesMutex);
            auto it = g_framebufferImages.find(info->framebuffer);
            if (it == g_framebufferImages.end()) return;
            fbImages = it->second;
        }
        // For each clear value, classify; if magic, tag the matching attachment
        // image (clear values are 1:1 with attachments in attachment order).
        uint32_t n = std::min<uint32_t>(info->clearValueCount, (uint32_t)fbImages.size());
        for (uint32_t i = 0; i < n; ++i) {
            int raw = ClassifyMagicColor(info->pClearValues[i].color);
            int eye = (raw >= 0) ? (raw % 10) : -1;
            if (eye >= 0 && fbImages[i] != VK_NULL_HANDLE) {
                bool newly = false;
                {
                    std::lock_guard<std::mutex> lk(g_imageEyeMutex);
                    auto it = g_imageEye.find(fbImages[i]);
                    if (it == g_imageEye.end()) {
                        g_imageEye[fbImages[i]] = eye;
                        newly = true;
                    } else {
                        it->second = eye;
                    }
                }
                if (newly) {
                    g_imageEyeIdentified[eye].fetch_add(1, std::memory_order_relaxed);
                    std::fprintf(stderr,
                        "[BetterVR-Linux] Magic clear (BeginRP cv[%u]): eye=%d image=%p\n",
                        i, eye, (void*)fbImages[i]);
                }
                // Also tag this cmdbuf with the eye in case the render pass
                // ends without going through our usual attachment-image check.
                std::lock_guard<std::mutex> lk(g_cmdbufEyeMutex);
                g_cmdbufEye[cb] = eye;
            }
        }
    }
    static void CmdBeginRenderPass(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkRenderPassBeginInfo*            pRenderPassBegin,
        VkSubpassContents                       contents)
    {
        g_beginRenderPassCount.fetch_add(1, std::memory_order_relaxed);
        if (pRenderPassBegin) {
            MarkCmdbufIfFbTargetsSwap(commandBuffer, pRenderPassBegin->framebuffer);
            HandleBeginRenderPassClearValues(commandBuffer, pRenderPassBegin);
        }
        pDispatch.CmdBeginRenderPass(commandBuffer, pRenderPassBegin, contents);
    }
    static void CmdBeginRenderPass2(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkRenderPassBeginInfo*            pRenderPassBegin,
        const VkSubpassBeginInfo*               pSubpassBeginInfo)
    {
        g_beginRenderPassCount.fetch_add(1, std::memory_order_relaxed);
        if (pRenderPassBegin) {
            MarkCmdbufIfFbTargetsSwap(commandBuffer, pRenderPassBegin->framebuffer);
            HandleBeginRenderPassClearValues(commandBuffer, pRenderPassBegin);
        }
        pDispatch.CmdBeginRenderPass2(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }
    static void CmdBeginRenderPass2KHR(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkRenderPassBeginInfo*            pRenderPassBegin,
        const VkSubpassBeginInfo*               pSubpassBeginInfo)
    {
        g_beginRenderPassCount.fetch_add(1, std::memory_order_relaxed);
        if (pRenderPassBegin) {
            MarkCmdbufIfFbTargetsSwap(commandBuffer, pRenderPassBegin->framebuffer);
            HandleBeginRenderPassClearValues(commandBuffer, pRenderPassBegin);
        }
        pDispatch.CmdBeginRenderPass2KHR(commandBuffer, pRenderPassBegin, pSubpassBeginInfo);
    }
    // Legacy render pass END variants need to check the same per-cmdbuf flag
    // and inject if it was set.
    static void CmdEndRenderPass2(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkSubpassEndInfo*                 pSubpassEndInfo)
    {
        int eye = TakeCmdbufEye(commandBuffer);
        pDispatch.CmdEndRenderPass2(commandBuffer, pSubpassEndInfo);
        g_endRenderingCount.fetch_add(1, std::memory_order_relaxed);
        if (eye != -2) {
            g_endRenderingSwapchainHits.fetch_add(1, std::memory_order_relaxed);
            InjectPerEyeCopy(pDispatch, commandBuffer, eye);
        }
    }
    static void CmdEndRenderPass2KHR(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkSubpassEndInfo*                 pSubpassEndInfo)
    {
        int eye = TakeCmdbufEye(commandBuffer);
        pDispatch.CmdEndRenderPass2KHR(commandBuffer, pSubpassEndInfo);
        g_endRenderingCount.fetch_add(1, std::memory_order_relaxed);
        if (eye != -2) {
            g_endRenderingSwapchainHits.fetch_add(1, std::memory_order_relaxed);
            InjectPerEyeCopy(pDispatch, commandBuffer, eye);
        }
    }

    // ---- Per-eye capture: track which swapchain image Cemu is rendering to.
    // Cemu calls vkAcquireNextImageKHR each frame; the returned image index
    // tells us which of g_cemuSwap.images is the current render target.
    static VkResult AcquireNextImageKHR(
        const vkroots::VkDeviceDispatch& pDispatch,
        VkDevice                         device,
        VkSwapchainKHR                   swapchain,
        uint64_t                         timeout,
        VkSemaphore                      semaphore,
        VkFence                          fence,
        uint32_t*                        pImageIndex)
    {
        VkResult r = pDispatch.AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
            VkSwapchainKHR tracked = VK_NULL_HANDLE;
            { std::lock_guard<std::mutex> lk(g_handlesMutex); tracked = g_cemuSwap.handle; }
            if (swapchain == tracked && pImageIndex) {
                g_activeSwapImageIndex.store(*pImageIndex, std::memory_order_release);
            }
        }
        return r;
    }

    // Begin dynamic rendering: scan color attachments, mark cmdbuf if any view
    // resolves to a swapchain image.
    static void CmdBeginRendering(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkRenderingInfo*                  pRenderingInfo)
    {
        g_beginRenderingCount.fetch_add(1, std::memory_order_relaxed);
        MarkCmdbufIfTargetsSwap(commandBuffer, pRenderingInfo);
        pDispatch.CmdBeginRendering(commandBuffer, pRenderingInfo);
    }
    static void CmdBeginRenderingKHR(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer,
        const VkRenderingInfo*                  pRenderingInfo)
    {
        g_beginRenderingCount.fetch_add(1, std::memory_order_relaxed);
        MarkCmdbufIfTargetsSwap(commandBuffer, pRenderingInfo);
        pDispatch.CmdBeginRenderingKHR(commandBuffer, pRenderingInfo);
    }

    // End dynamic rendering / render pass: if the cmdbuf was marked as targeting
    // the swapchain, inject our copy command before the next render pass.
    static void CmdEndRendering(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer)
    {
        int eye = TakeCmdbufEye(commandBuffer);
        pDispatch.CmdEndRendering(commandBuffer);
        g_endRenderingCount.fetch_add(1, std::memory_order_relaxed);
        if (eye != -2) {
            g_endRenderingSwapchainHits.fetch_add(1, std::memory_order_relaxed);
            InjectPerEyeCopy(pDispatch, commandBuffer, eye);
        }
    }
    static void CmdEndRenderingKHR(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer)
    {
        int eye = TakeCmdbufEye(commandBuffer);
        pDispatch.CmdEndRenderingKHR(commandBuffer);
        g_endRenderingCount.fetch_add(1, std::memory_order_relaxed);
        if (eye != -2) {
            g_endRenderingSwapchainHits.fetch_add(1, std::memory_order_relaxed);
            InjectPerEyeCopy(pDispatch, commandBuffer, eye);
        }
    }
    static void CmdEndRenderPass(
        const vkroots::VkCommandBufferDispatch& pDispatch,
        VkCommandBuffer                         commandBuffer)
    {
        int eye = TakeCmdbufEye(commandBuffer);
        pDispatch.CmdEndRenderPass(commandBuffer);
        g_endRenderingCount.fetch_add(1, std::memory_order_relaxed);
        if (eye != -2) {
            g_endRenderingSwapchainHits.fetch_add(1, std::memory_order_relaxed);
            InjectPerEyeCopy(pDispatch, commandBuffer, eye);
        }
    }

private:
    static void CopyPresentToSharedIfReady(const VkPresentInfoKHR* presentInfo);
    static void InjectPerEyeCopy(const vkroots::VkCommandBufferDispatch& dd, VkCommandBuffer cb, int eye);
    static void InjectPreClearCapture(const vkroots::VkCommandBufferDispatch& dd,
                                      VkCommandBuffer cb, VkImage src,
                                      VkImageLayout srcLayout, int layer, int eye);

    // Decide the per-cmdbuf eye marker from a list of attachment images.
    //   Returns -1 if any attachment is a swapchain image — used to mean
    //     "inject the swapchain into eye 0 at EndRendering" but we now leave
    //     that path off: per-eye capture is handled by the pre-clear capture
    //     in CmdClearColorImage instead.
    //   Returns -2 (no marker) if nothing matches.
    // We deliberately DO NOT mark cmdbufs based on g_imageEye anymore — that
    // was causing EndRendering's InjectPerEyeCopy to overwrite our pre-clear
    // capture with the (green) swapchain content.
    static int ClassifyAttachmentImages(const std::vector<VkImage>& imgs) {
        std::vector<VkImage> swapImages;
        {
            std::lock_guard<std::mutex> lk(g_handlesMutex);
            swapImages = g_cemuSwap.images;
        }
        for (VkImage img : imgs) {
            for (VkImage swap : swapImages) {
                if (img == swap) return -1;
            }
        }
        return -2;
    }

    // Helper: any of these images known to be an eye image? If yes, mark them
    // "dirty" so the next magic clear knows there's been a render since the
    // last capture.
    static void MarkImagesDirtyIfEye(const std::vector<VkImage>& imgs) {
        std::lock_guard<std::mutex> lk1(g_imageEyeMutex);
        std::lock_guard<std::mutex> lk2(g_imageDirtyMutex);
        for (VkImage img : imgs) {
            if (g_imageEye.find(img) != g_imageEye.end()) {
                g_imageDirty[img] = true;
            }
        }
    }

    // For dynamic rendering's VkRenderingInfo: resolve color attachment views
    // to images, then classify.
    static void MarkCmdbufIfTargetsSwap(VkCommandBuffer cb, const VkRenderingInfo* info) {
        if (!info || info->colorAttachmentCount == 0) return;
        std::vector<VkImage> imgs;
        imgs.reserve(info->colorAttachmentCount);
        {
            std::lock_guard<std::mutex> lk(g_viewToImageMutex);
            for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
                VkImageView v = info->pColorAttachments[i].imageView;
                if (v == VK_NULL_HANDLE) continue;
                auto it = g_viewToImage.find(v);
                if (it != g_viewToImage.end()) imgs.push_back(it->second);
            }
        }
        MarkImagesDirtyIfEye(imgs);
        int eye = ClassifyAttachmentImages(imgs);
        if (eye == -2) return;
        std::lock_guard<std::mutex> lk(g_cmdbufEyeMutex);
        g_cmdbufEye[cb] = eye;
    }

    // Helper: read+clear the per-cmdbuf eye marker.
    // Returns -2 if no marker set. Otherwise -1/0/1.
    static int TakeCmdbufEye(VkCommandBuffer cb) {
        std::lock_guard<std::mutex> lk(g_cmdbufEyeMutex);
        auto it = g_cmdbufEye.find(cb);
        if (it == g_cmdbufEye.end()) return -2;
        int v = it->second;
        g_cmdbufEye.erase(it);
        return v;
    }

    // Helper for legacy render-pass begin: look up the framebuffer's
    // attachment images and classify them.
    static void MarkCmdbufIfFbTargetsSwap(VkCommandBuffer cb, VkFramebuffer fb) {
        if (fb == VK_NULL_HANDLE) return;
        std::vector<VkImage> fbImages;
        {
            std::lock_guard<std::mutex> lk(g_framebufferImagesMutex);
            auto it = g_framebufferImages.find(fb);
            if (it == g_framebufferImages.end()) return;
            fbImages = it->second;
        }
        if (fbImages.empty()) return;
        MarkImagesDirtyIfEye(fbImages);
        int eye = ClassifyAttachmentImages(fbImages);
        if (eye == -2) return;
        std::lock_guard<std::mutex> lk(g_cmdbufEyeMutex);
        g_cmdbufEye[cb] = eye;
    }
};

// Long-lived per-thread resources for the present-side copy. Stored as static
// locals inside the function so they only allocate once per thread.
struct PresentCopyResources {
    VkCommandPool   pool = VK_NULL_HANDLE;
    VkCommandBuffer cmd  = VK_NULL_HANDLE;
    VkQueue         queue = VK_NULL_HANDLE;
    const vkroots::VkDeviceDispatch* dd = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    bool     ok = false;
};

void VkDeviceOverrides::CopyPresentToSharedIfReady(const VkPresentInfoKHR* presentInfo) {
    // Disabled since the in-cmdbuf InjectPerEyeCopy now does the per-eye
    // capture at the right moment. Leaving this present-time copy active would
    // clobber whichever eye was last captured with a mono "latest swapchain"
    // copy, defeating the per-eye split.
    (void)presentInfo;
    return;

#if 0
    if (!presentInfo || presentInfo->swapchainCount == 0) return;

    CemuSwapchain swap;
    SharedImage   shared;
    CapturedHandles h;
    {
        std::lock_guard<std::mutex> lock(g_handlesMutex);
        swap   = g_cemuSwap;
        shared = g_eyeImages[0];
        h      = g_handles;
    }
    if (shared.cemuImage == VK_NULL_HANDLE) {
        g_hookState.presentBlitsSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (swap.handle == VK_NULL_HANDLE || swap.images.empty()) {
        g_hookState.presentBlitsSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (presentInfo->pSwapchains[0] != swap.handle) {
        g_hookState.presentBlitsSkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t imgIdx = presentInfo->pImageIndices[0];
    if (imgIdx >= swap.images.size()) return;
    VkImage srcImage = swap.images[imgIdx];

    thread_local PresentCopyResources pc;
    if (!pc.ok) {
        pc.dd     = vkroots::tables::DeviceDispatches.find(h.device);
        pc.device = h.device;
        if (!pc.dd) return;
        pc.dd->GetDeviceQueue(h.device, h.graphicsQueueFamily, 0, &pc.queue);

        VkCommandPoolCreateInfo cpci = {};
        cpci.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = h.graphicsQueueFamily;
        if (pc.dd->CreateCommandPool(h.device, &cpci, nullptr, &pc.pool) != VK_SUCCESS) return;

        VkCommandBufferAllocateInfo cbai = {};
        cbai.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbai.commandPool        = pc.pool;
        cbai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbai.commandBufferCount = 1;
        if (pc.dd->AllocateCommandBuffers(h.device, &cbai, &pc.cmd) != VK_SUCCESS) return;

        pc.ok = true;
        std::fprintf(stderr, "[BetterVR-Linux] PresentCopy: command resources ready on tid\n");
    }

    pc.dd->ResetCommandBuffer(pc.cmd, 0);
    VkCommandBufferBeginInfo cbbi = {};
    cbbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    pc.dd->BeginCommandBuffer(pc.cmd, &cbbi);

    VkImageSubresourceRange one = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // src: PRESENT_SRC_KHR -> TRANSFER_SRC_OPTIMAL
    VkImageMemoryBarrier b1 = {};
    b1.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.srcAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    b1.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b1.oldLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b1.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image               = srcImage;
    b1.subresourceRange    = one;
    // dst: TRANSFER_SRC_OPTIMAL (current) -> TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier b2 = {};
    b2.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b2.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image               = shared.cemuImage;
    b2.subresourceRange    = one;
    VkImageMemoryBarrier pre[] = { b1, b2 };
    pc.dd->CmdPipelineBarrier(pc.cmd,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 2, pre);

    // BVR_CROP env var picks which subrect of Cemu's swapchain to display.
    //   "left"   = src.x: [0, width/2)        — side-by-side stereo, left eye
    //   "right"  = src.x: [width/2, width)    — side-by-side stereo, right eye
    //   "top"    = src.y: [0, height/2)       — top-bottom stereo, top eye
    //   "bottom" = src.y: [height/2, height)  — top-bottom stereo, bottom eye
    //   anything else / unset = full swapchain
    static const std::string kCrop = [](){
        const char* v = std::getenv("BVR_CROP");
        std::string s = v ? std::string(v) : std::string("");
        std::fprintf(stderr, "[BetterVR-Linux] BVR_CROP=%s\n", s.empty() ? "(unset)" : s.c_str());
        return s;
    }();
    int32_t srcX0 = 0, srcY0 = 0, srcX1 = (int32_t)swap.width, srcY1 = (int32_t)swap.height;
    if (kCrop == "left")        { srcX1 = (int32_t)(swap.width / 2); }
    else if (kCrop == "right")  { srcX0 = (int32_t)(swap.width / 2); }
    else if (kCrop == "top")    { srcY1 = (int32_t)(swap.height / 2); }
    else if (kCrop == "bottom") { srcY0 = (int32_t)(swap.height / 2); }

    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0]  = { srcX0, srcY0, 0 };
    blit.srcOffsets[1]  = { srcX1, srcY1, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1]  = { (int32_t)shared.width, (int32_t)shared.height, 1 };
    pc.dd->CmdBlitImage(pc.cmd,
        srcImage,          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        shared.cemuImage,  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    // src: TRANSFER_SRC_OPTIMAL -> PRESENT_SRC_KHR (let Cemu's present continue)
    // dst: TRANSFER_DST_OPTIMAL -> TRANSFER_SRC_OPTIMAL (for OpenXR side to blit)
    VkImageMemoryBarrier b3 = {};
    b3.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b3.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b3.dstAccessMask       = VK_ACCESS_MEMORY_READ_BIT;
    b3.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b3.newLayout           = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b3.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b3.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b3.image               = srcImage;
    b3.subresourceRange    = one;
    VkImageMemoryBarrier b4 = {};
    b4.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b4.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b4.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b4.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b4.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b4.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b4.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b4.image               = shared.cemuImage;
    b4.subresourceRange    = one;
    VkImageMemoryBarrier post[] = { b3, b4 };
    pc.dd->CmdPipelineBarrier(pc.cmd,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr, 2, post);

    pc.dd->EndCommandBuffer(pc.cmd);

    VkSubmitInfo si = {};
    si.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &pc.cmd;
    pc.dd->QueueSubmit(pc.queue, 1, &si, VK_NULL_HANDLE);
    pc.dd->DeviceWaitIdle(pc.device);
    g_hookState.presentBlits.fetch_add(1, std::memory_order_relaxed);
#endif
}

// Injected into Cemu's command buffer at vkCmdEndRendering / EndRenderPass.
// Copies the currently-acquired swapchain image into the per-eye shared image
// (selected by g_currentEye) so the OpenXR side has a per-eye capture even
// though the SAME swapchain image is about to be overwritten by the next eye's
// render pass.
//
// Filter: only fires when MarkCmdbufIfTargetsSwap / MarkCmdbufIfFbTargetsSwap
// set the per-cmdbuf flag earlier in the recording (so we don't waste GPU work
// copying intermediate render targets).
void VkDeviceOverrides::InjectPerEyeCopy(const vkroots::VkCommandBufferDispatch& dd, VkCommandBuffer cb, int eye) {
    // Disabled: this path used to fire at every render-pass END that targeted
    // an eye image or the swapchain, copying the swapchain image (which is
    // all-green after BetterVR's magic clears) into the per-eye targets and
    // clobbering our good pre-clear captures. Per-eye capture is now handled
    // exclusively by InjectPreClearCapture in CmdClearColorImage.
    (void)dd; (void)cb; (void)eye;
    return;
#if 0
    if (eye < 0 || eye > 1) {
        eye = g_currentEye.load(std::memory_order_acquire);
        if (eye < 0 || eye > 1) eye = 0;
    }
    SharedImage   shared;
    CemuSwapchain swap;
    {
        std::lock_guard<std::mutex> lock(g_handlesMutex);
        shared = g_eyeImages[eye];
        swap   = g_cemuSwap;
    }
    if (shared.cemuImage == VK_NULL_HANDLE) return;
    if (swap.handle == VK_NULL_HANDLE || swap.images.empty()) return;
    uint32_t idx = g_activeSwapImageIndex.load(std::memory_order_acquire);
    if (idx >= swap.images.size()) return;
    VkImage srcImage = swap.images[idx];

    VkImageSubresourceRange one = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    // We're being injected INSIDE Cemu's recording. The swapchain image is in
    // COLOR_ATTACHMENT_OPTIMAL (just left a render pass). Transition to
    // TRANSFER_SRC for the copy, then back to COLOR_ATTACHMENT so Cemu's next
    // operation sees the layout it expects.
    VkImageMemoryBarrier b1 = {};
    b1.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b1.srcAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b1.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b1.oldLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b1.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b1.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b1.image               = srcImage;
    b1.subresourceRange    = one;
    VkImageMemoryBarrier b2 = {};
    b2.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b2.srcAccessMask       = 0;
    b2.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    b2.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b2.image               = shared.cemuImage;
    b2.subresourceRange    = one;
    VkImageMemoryBarrier pre[] = { b1, b2 };
    dd.CmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 2, pre);

    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1]  = { (int32_t)swap.width, (int32_t)swap.height, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1]  = { (int32_t)shared.width, (int32_t)shared.height, 1 };
    dd.CmdBlitImage(cb,
        srcImage,         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        shared.cemuImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    // Restore swapchain image layout for Cemu's next op; we leave the dest in
    // TRANSFER_DST_OPTIMAL because we'll only re-use it as TRANSFER_DST for the
    // next inject (cross-instance reads use OPAQUE_FD shared memory directly).
    VkImageMemoryBarrier b3 = {};
    b3.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b3.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    b3.dstAccessMask       = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b3.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b3.newLayout           = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b3.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b3.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b3.image               = srcImage;
    b3.subresourceRange    = one;
    dd.CmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
        0, nullptr, 0, nullptr, 1, &b3);

    g_injectedCopies.fetch_add(1, std::memory_order_relaxed);
    g_injectedCopiesPerEye[eye].fetch_add(1, std::memory_order_relaxed);
#endif
}

// Capture the source image's current content into g_eyeImages[eye] BEFORE the
// caller's vkCmdClearColorImage wipes it. We need to:
//   1) transition src from its current layout to TRANSFER_SRC
//   2) transition dst (eye image) to TRANSFER_DST
//   3) blit
//   4) restore src to its caller-provided layout so the upcoming clear works
//      (it expects whatever Cemu set)
void VkDeviceOverrides::InjectPreClearCapture(const vkroots::VkCommandBufferDispatch& dd,
                                              VkCommandBuffer cb, VkImage src,
                                              VkImageLayout srcLayout, int layer, int eye)
{
    if (layer < 0 || layer > 1 || eye < 0 || eye > 1) return;
    SharedImage shared;
    {
        std::lock_guard<std::mutex> lk(g_handlesMutex);
        shared = g_eyeImages[layer][eye];
    }
    if (shared.cemuImage == VK_NULL_HANDLE) return;

    // Pair this capture with the head pose BotW used to render its content.
    // Pop the per-eye pose queue (filled by Hook_GetRenderCamera). If the
    // queue is empty (capture happened without a matching camera-hook fire,
    // e.g., menu-only frames), fall back to the latest known pose. Also
    // flag this eye as fresh — the FrameLoop won't consume until BOTH
    // eyes' flags are set (port of upstream's Is3DComplete() gate).
    {
        std::lock_guard<std::mutex> lk(g_capturedPoseMutex);
        if (!g_renderedPoseQueue[eye].empty()) {
            g_capturedPose[layer][eye] = g_renderedPoseQueue[eye].front();
            g_renderedPoseQueue[eye].pop_front();
            g_capturedPoseValid[layer][eye] = true;
        } else {
            XrPosef fallback = {};
            if (g_hookState.getPose(eye, &fallback)) {
                g_capturedPose[layer][eye] = fallback;
                g_capturedPoseValid[layer][eye] = true;
            }
        }
        if (layer == 0) g_imageFresh[eye] = true;
    }

    VkImageSubresourceRange one = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    VkImageMemoryBarrier toSrc = {};
    toSrc.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toSrc.srcAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
    toSrc.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toSrc.oldLayout           = srcLayout;
    toSrc.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toSrc.image               = src;
    toSrc.subresourceRange    = one;
    VkImageMemoryBarrier toDst = {};
    toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toDst.srcAccessMask       = 0;
    toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
    toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toDst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toDst.image               = shared.cemuImage;
    toDst.subresourceRange    = one;
    VkImageMemoryBarrier pre[] = { toSrc, toDst };
    dd.CmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
        0, nullptr, 0, nullptr, 2, pre);

    // Look up the source image's actual extent (recorded by our vkCreateImage
    // hook). Without this we'd blit g_cemuSwap dims (the final swapchain size)
    // which is wrong for post-HDR composed render textures — those are at
    // BotW's internal render resolution and out-of-bounds reads produce the
    // "1/4 in top-left + stretched edges" visual artifact.
    int32_t srcW = 0, srcH = 0;
    {
        std::lock_guard<std::mutex> lk(g_imageExtentMutex);
        auto it = g_imageExtent.find(src);
        if (it != g_imageExtent.end()) { srcW = (int32_t)it->second.w; srcH = (int32_t)it->second.h; }
    }
    if (srcW <= 0 || srcH <= 0) {
        // Fallback if we don't have the extent (shouldn't happen).
        CemuSwapchain swap;
        { std::lock_guard<std::mutex> lk(g_handlesMutex); swap = g_cemuSwap; }
        srcW = swap.width  > 0 ? (int32_t)swap.width  : (int32_t)shared.width;
        srcH = swap.height > 0 ? (int32_t)swap.height : (int32_t)shared.height;
    }

    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[1]  = { srcW, srcH, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[1]  = { (int32_t)shared.width, (int32_t)shared.height, 1 };
    dd.CmdBlitImage(cb,
        src,              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        shared.cemuImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        1, &blit, VK_FILTER_LINEAR);

    // Restore src layout for the upcoming vkCmdClearColorImage.
    VkImageMemoryBarrier toOrig = {};
    toOrig.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toOrig.srcAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
    toOrig.dstAccessMask       = VK_ACCESS_MEMORY_WRITE_BIT;
    toOrig.oldLayout           = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toOrig.newLayout           = srcLayout;
    toOrig.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toOrig.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toOrig.image               = src;
    toOrig.subresourceRange    = one;
    dd.CmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
        0, nullptr, 0, nullptr, 1, &toOrig);

    g_injectedCopies.fetch_add(1, std::memory_order_relaxed);
    g_injectedCopiesPerEye[eye].fetch_add(1, std::memory_order_relaxed);
}

} // namespace bvr_linux

VKROOTS_DEFINE_LAYER_INTERFACES(bvr_linux::VkInstanceOverrides,
                                bvr_linux::VkDeviceOverrides);
