[BetterVR_RND_Logging_V208]
moduleMatches = 0x6267BFD0

.origin = codecave


; check 0x39b2668 (gsys::ModelSceneBuffer::endDeferredShading) and 0x399b904 (gsys::ModelRenderContext::clear) and 0x39b11c0 (gsys::ModelSceneBuffer::calcView) and 0x39b2578 (gsys::ModelSceneBuffer::beginDeferredShading)
; writes zero at 0x399b904 (gsys::ModelRenderContext::clear)

hook_calcView_str:
.string "calcView() wrote %08X to the RenderSceneContext"

hook_calcView_setsRenderBuffer:
stw r12, 0x1B0(r26)

mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)
stw r7, 0x0C(r1)
stw r8, 0x08(r1)

lis r5, hook_calcView_str@ha
addi r5, r5, hook_calcView_str@l
mr r6, r12
bl printToCemuConsoleWithFormat

lwz r8, 0x08(r1)
lwz r7, 0x0C(r1)
lwz r6, 0x10(r1)
lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

0x039B11C0 = bla hook_calcView_setsRenderBuffer

; ================================================================================

hook_beginDeferredShading_str:
.string "beginDeferredShading() wrote %08X to the RenderSceneContext"

hook_beginDeferredShading_setsRenderBuffer:
stw r0, 0x1B0(r30)

mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)

lis r5, hook_beginDeferredShading_str@ha
addi r5, r5, hook_beginDeferredShading_str@l
lwz r6, 0x1B0(r30)
bl printToCemuConsoleWithFormat

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
lwz r6, 0x10(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

0x39b2578 = bla hook_beginDeferredShading_setsRenderBuffer

; ================================================================================

hook_endDeferredShading_str:
.string "endDeferredShading() wrote %08X to the RenderSceneContext"

hook_endDeferredShading_setsRenderBuffer:
stw r0, 0x1B0(r31)

mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)

lis r5, hook_endDeferredShading_str@ha
addi r5, r5, hook_endDeferredShading_str@l
lwz r6, 0x1B0(r31)
bl printToCemuConsoleWithFormat

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
lwz r6, 0x10(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

0x39b2668 = bla hook_endDeferredShading_setsRenderBuffer

; ================================================================================

hook_clear_str:
.string "clear() wrote %08X to the RenderSceneContext"

hook_clear_setsRenderBuffer:
stw r11, 0x1B0(r3)

mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)

lis r5, hook_clear_str@ha
addi r5, r5, hook_clear_str@l
mr r6, r11
bl printToCemuConsoleWithFormat

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
lwz r6, 0x10(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
add r0, r8, r12
blr

0x0399B904 = bla hook_clear_setsRenderBuffer

; ================================================================================

strSubStep_SystemTask_invoke:
.string "gsys::SystemTask::invoke( %08X )"

custom_sead_Delegate_gsys_SystemTask_invoke:
mr r12, r3
lwz r11, 4(r12)
cmpwi r11, 0
beqlr
lha r0, 0xA(r12)
cmpwi r0, 0
beqlr
lha r10, 8(r12)
cmpwi r0, 0
add r3, r11, r10
bge other_unused_delegate_jump

; --- LOGGING ---
stwu r1, -0x20(r1)
mflr r11
stw r11, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)

lis r5, strSubStep_SystemTask_invoke@ha
addi r5, r5, strSubStep_SystemTask_invoke@l
lwz r6, 0xC(r12)
bl printToCemuConsoleWithFormat

lwz r11, 0x24(r1)
mtlr r11
lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
lwz r6, 0x10(r1)
addi r1, r1, 0x20
; --- LOGGING ---

lwz r11, 0xC(r12)
mtctr r11
; gsys__SystemTask__preCalc_ gsys__SystemTask__postCalc_ gsys__SystemTask__drawTV_ ... # Dst: 0x03a128d4, 0x03a135a0, 0x03a146e0, 0x03a14804, 0x03a148d4, 0x03a14970
bctr

; this is basically not used
other_unused_delegate_jump:
lha r12, 0xE(r12)
lwzx r10, r3, r12
slwi r0, r0, 3
add r9, r10, r0
lwz r11, 4(r9)
mtctr r11
bctr

0x03A14D74 = ba custom_sead_Delegate_gsys_SystemTask_invoke

; ================================================================================

perspectiveProjection_setFovY_formatStr:
.string "sead::PerspectiveProjection::setFovY(this = 0x%08x fovRadians = %f offsetX = %f near = %f far = %f) LR = 0x%08x"
printSetFovY:
mflr r0
stwu r1, -0x40(r1)
stw r0, 0x44(r1)
stw r3, 0x3C(r1)
stw r4, 0x38(r1)
stw r5, 0x34(r1)
stw r6, 0x30(r1)
stw r7, 0x2C(r1)
stw r8, 0x28(r1)

stfs f1, 0x20(r1)
stfs f2, 0x1C(r1)
stfs f3, 0x18(r1)
stfs f4, 0x14(r1)

lfs f2, 0xB0(r3) ; load offsetX
lfs f3, 0x94(r3) ; load near
lfs f4, 0x98(r3) ; load far

mr r7, r3
addi r1, r1, 0x40
lwz r6, 0x10(r1) ; load LR from parent stack
stwu r1, -0x40(r1)
lis r5, perspectiveProjection_setFovY_formatStr@ha
addi r5, r5, perspectiveProjection_setFovY_formatStr@l
bl printToCemuConsoleWithFormat

fmuls f31, f1, f0


lfs f4, 0x20(r1)
lfs f3, 0x1C(r1)
lfs f2, 0x18(r1)
lfs f1, 0x14(r1)

lwz r3, 0x3C(r1)
lwz r4, 0x38(r1)
lwz r5, 0x34(r1)
lwz r6, 0x30(r1)
lwz r7, 0x2C(r1)
lwz r8, 0x28(r1)

lwz r0, 0x44(r1)
mtlr r0
addi r1, r1, 0x40
blr

;0x030C16F8 = bla printSetFovY

; ================================================================================

formatLayer3DDrawingStepsStr:
.string "gsys::Layer3D::draw( this = %08X, step = %d )"

logLayer3DDrawSteps:
mflr r11
stwu r1, -0x20(r1)
stw r11, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)
stw r7, 0x0C(r1)
stw r8, 0x08(r1)

lwz r11, 0x0(r30) ; original instruction

lis r5, formatLayer3DDrawingStepsStr@ha
addi r5, r5, formatLayer3DDrawingStepsStr@l
lwz r6, 0x1C(r1)
mr r7, r11
bl printToCemuConsoleWithFormat

lwz r8, 0x08(r1)
lwz r7, 0x0C(r1)
lwz r6, 0x10(r1)
lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r11, 0x24(r1)
addi r1, r1, 0x20
mtlr r11

lwz r11, 0x0(r30) ; original instruction
blr

0x0397A9A0 = bla logLayer3DDrawSteps

; =======================================================================================================================

format_layerJobStr:
.string "agl::lyr::LayerJob::pushBackTo( this = %08X, layer = %08X, layerName = %s, renderDisplay = %08X )"

unknownLayerNameStr:
.string "Unknown Layer Name"

hook_lyr_LayerJob_pushbackJob:
stw r4, 0x14(r3)

mflr r4
stwu r1, -0x20(r1)
stw r4, 0x24(r1)
lwz r4, 0x1C(r3)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)
stw r6, 0x10(r1)
stw r7, 0x0C(r1)
stw r8, 0x08(r1)
stw r9, 0x04(r1)


lwz r6, 0x1C(r1)
lwz r7, 0x10(r6)
cmpwi r7, 0
lis r8, unknownLayerNameStr@ha
addi r8, r8, unknownLayerNameStr@l
beq dontReadLayerName
addi r8, r7, 0x9C ; layer name

dontReadLayerName:
lis r5, format_layerJobStr@ha
addi r5, r5, format_layerJobStr@l
lwz r7, 0x1C(r1)
lwz r9, 0x14(r7)
; r5 = format_layerJobStr
; r6 = LayerJob* this
; r7 = LayerJob->layer*
; r8 = LayerJob->layer->name*
; r9 = LayerJob->renderDisplay*
bl printToCemuConsoleWithFormat

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
lwz r6, 0x10(r1)
lwz r7, 0x0C(r1)
lwz r8, 0x08(r1)
lwz r9, 0x04(r1)
lwz r4, 0x24(r1)
addi r1, r1, 0x20
mtlr r4

cmpwi r6, 0
stw r5, 0x18(r3)
beq loc_3B4B4E0
lwz r12, 0(r6)
lwz r0, 4(r6)
cmpw r12, r0
bgelr
lwz r0, 8(r6)
slwi r10, r12, 2
stwx r3, r10, r0
lwz r12, 0(r6)
addi r12, r12, 1
stw r12, 0(r6)
blr
loc_3B4B4E0:
lwz r12, 8(r3)
lwz r10, 0x14(r12)
mtctr r10
bctr # agl__lyr__LayerJob__invoke
blr

0x03B4B4A4 = ba hook_lyr_LayerJob_pushbackJob

; ===============================================================================
; LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT
; RND_Renderer::StartFrame
; [Meta XR Simulator][00066.853197][V][arvr\projects\openxr_simulator\src\sim_xrsession.cpp:260] xrWaitFrame: missed frame interval: previous interval=4808, current interval=4813, time diff = 66.078ms
; [Meta XR Simulator][00066.861202][V][arvr\projects\openxr_simulator\src\session_capture\sim_session_capturer_gfx.cpp:95] openxr_simulator::SessionCaptureRecorderGfx::beginFrame
; Updating actions while being in the in-game state
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; Rendering layer 0...
; calcView() wrote 28C05E3D to the RenderSceneContext
; Rendering layer 33554432...
; Rendering layer 50331648...
; Rendering layer 67108864...
; beginDeferredShading() wrote E4FA5E3D to the RenderSceneContext
; endDeferredShading() wrote 28C05E3D to the RenderSceneContext
; Rendering layer 83886080...
; Rendering layer 100663296...
; Rendering layer 117440512...
; Rendering layer 134217728...
; Rendering layer 150994944...
; Rendering layer 167772160...
; Rendering layer 16777216...
; [VULKAN] Clearing color image for 3D layer for left side (0)
; [VULKAN] Color image is not the same as the current 3D color image! (0x214668d3b20 != 0x2144bea5c50)
; [VULKAN] Clearing color image for 3D layer for left side (0)
; [VULKAN] Queueing up a 3D_COLOR signal inside cmd buffer 0x21408562220 for left side
; [VULKAN] Clearing depth image for 3D layer for left side (1)
; [VULKAN] Depth image is not the same as the current 3D depth image! (0x2144d7f47b0 != 0x2144bea3aa0)
; [VULKAN] Clearing depth image for 3D layer for left side (1)
; [VULKAN] Queueing up a DEPTH signal inside cmd buffer 0x21408562220 for left side
; [VULKAN] QueueSubmit called with 2 active copy operations
; [VULKAN] Waiting for Layer3D - Left Color Texture to be 0 inside the cmd buffer 0x21408562220
; [VULKAN] Signalling to Layer3D - Left Color Texture to be 1 inside the cmd buffer 0x21408562220
; [VULKAN] Waiting for Layer3D - Left Depth Texture to be 0 inside the cmd buffer 0x21408562220
; [VULKAN] Signalling to Layer3D - Left Depth Texture to be 1 inside the cmd buffer 0x21408562220
; [VULKAN] Clearing color image for 2D layer for left side (0)
; [VULKAN] Queueing up a 2D_COLOR signal inside cmd buffer 0x214085bbbd0 for left side
; [VULKAN] Clearing color image for 2D layer for left side (0)
; [PPC] Clearing 2D color buffer with left eye
; LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT
; ===============================================================================
;
; ===============================================================================
; RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; clear() wrote 00000000 to the RenderSceneContext
; Rendering layer 0...
; calcView() wrote 28C05E3D to the RenderSceneContext
; Rendering layer 33554432...
; Rendering layer 50331648...
; Rendering layer 67108864...
; beginDeferredShading() wrote E4FA5E3D to the RenderSceneContext
; endDeferredShading() wrote 28C05E3D to the RenderSceneContext
; Rendering layer 83886080...
; Rendering layer 100663296...
; Rendering layer 117440512...
; Rendering layer 134217728...
; Rendering layer 150994944...
; Rendering layer 167772160...
; Rendering layer 16777216...
; [VULKAN] QueueSubmit called with 1 active copy operations
; [VULKAN] Waiting for Layer2D - Color Texture to be 0 inside the cmd buffer 0x214085bbbd0
; [VULKAN] Signalling to Layer2D - Color Texture to be 1 inside the cmd buffer 0x214085bbbd0
; [VULKAN] Clearing color image for 3D layer for right side (1)
; [VULKAN] Color image is not the same as the current 3D color image! (0x214668d3b20 != 0x2144bea5c50)
; [VULKAN] Clearing color image for 3D layer for right side (1)
; [VULKAN] Queueing up a 3D_COLOR signal inside cmd buffer 0x214085b87b0 for right side
; [VULKAN] Clearing depth image for 3D layer for right side (2)
; [VULKAN] Depth image is not the same as the current 3D depth image! (0x2144d7f47b0 != 0x2144bea3aa0)
; [VULKAN] Clearing depth image for 3D layer for right side (2)
; [VULKAN] Queueing up a DEPTH signal inside cmd buffer 0x214085b87b0 for right side
; [VULKAN] QueueSubmit called with 2 active copy operations
; [VULKAN] Waiting for Layer3D - Right Color Texture to be 0 inside the cmd buffer 0x214085b87b0
; [VULKAN] Signalling to Layer3D - Right Color Texture to be 1 inside the cmd buffer 0x214085b87b0
; [VULKAN] Waiting for Layer3D - Right Depth Texture to be 0 inside the cmd buffer 0x214085b87b0
; [VULKAN] Signalling to Layer3D - Right Depth Texture to be 1 inside the cmd buffer 0x214085b87b0
; [VULKAN] Clearing color image for 2D layer for left side (0)
; [VULKAN] Clearing color image for 2D layer for left side (0)
; [PPC] Clearing 2D color buffer with right eye
; RND_Renderer::EndFrame
; Presenting 2D layer
; [D3D12 - 2D Layer] Waiting for 2D layer's texture to be 1
; [D3D12 - 2D Layer] Signalling for 2D layer's texture to be 0
; [D3D12 - 2D Layer] Rendering finished
; Presenting 3D layer
; [D3D12] Waiting for 3D layer's left side to be 1
; [D3D12 - 3D Layer] Signalling for 3D layer's left side to be 0
; [D3D12] Waiting for 3D layer's right side to be 1
; [D3D12 - 3D Layer] Signalling for 3D layer's right side to be 0
; [Meta XR Simulator][00066.917700][V][arvr\projects\openxr_simulator\src\sim_xrsession.cpp:459] frameIndex 10580: creationTime 66853.193ms, beginTime 66861.200ms, endTime 66917.692ms, PDT 66930.633ms, PDP 13.889ms, shouldRender 1
; [Meta XR Simulator][00066.917919][V][arvr\projects\openxr_simulator\src\sim_xrapilayer_debug_window_base.cpp:213]   2 layers
; [Meta XR Simulator][00066.918007][V][arvr\projects\openxr_simulator\src\sim_xrapilayer_debug_window_base.cpp:216]     layer 0: XR_TYPE_COMPOSITION_LAYER_QUAD
; [Meta XR Simulator][00066.918107][V][arvr\projects\openxr_simulator\src\sim_xrapilayer_debug_window_base.cpp:216]     layer 1: XR_TYPE_COMPOSITION_LAYER_PROJECTION
; [Meta XR Simulator][00066.918652][V][arvr\projects\openxr_simulator\src\session_capture\sim_session_capturer_gfx.cpp:117] openxr_simulator::SessionCaptureRecorderGfx::endFrame
; RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT RIGHT
; ===============================================================================
;
; ===============================================================================
; LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT LEFT
; ... repeated logs for multiple frames ...