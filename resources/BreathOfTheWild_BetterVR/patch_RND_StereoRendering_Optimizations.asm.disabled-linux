[BetterVR_StereoRendering_Optimizations_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

; ==================================================================================
; Swap GX2SwapScanBuffers and GX2DrawDone calls around to allow VR headset to wait for vsync, and then actually sync the GPU for the next frame using GX2DrawDone, which should reduce stuttering and input lag
0x031FAB24 = returnAddr_doGx2DrawDoneAfterSwapBuffers:

doGx2DrawDoneAfterSwapBuffers:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)
stw r4, 0x08(r1)

lwz r3, 0x0C(r1)
bl import.gx2.GX2SwapScanBuffers

lwz r3, 0x0C(r1)
bl import.gx2.GX2DrawDone

lis r3, returnAddr_doGx2DrawDoneAfterSwapBuffers@ha
addi r3, r3, returnAddr_doGx2DrawDoneAfterSwapBuffers@l
mtlr r3

lwz r4, 0x08(r1)
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
;mtlr r0
blr

0x031FAB1C = bla doGx2DrawDoneAfterSwapBuffers
0x031FAA10 = nop

; --------------------------------------------------------------------------------
; Patches below disable the shadow map projection matrices updates to prevent a mismatch when applying the old one

hook_skipShadowUpdateShadowMatrix:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)
stw r4, 0x08(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
cmpwi r3, 1
beq exit_SkipShadowCalcView

0x03B0ACD0 = depthShadow_updateShadowMatrix:
lis r3, depthShadow_updateShadowMatrix@ha
addi r3, r3, depthShadow_updateShadowMatrix@l
mtctr r3
lwz r3, 0x0C(r1)
bctrl ; bl depthShadow_updateShadowMatrix

exit_SkipShadowCalcView:
lwz r4, 0x08(r1)
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x039DE028 = bla hook_skipShadowUpdateShadowMatrix


hook_skipShadowCalcViewMasked:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)
stw r4, 0x08(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
cmpwi r3, 1
beq exit_SkipShadowCalcViewMasked

0x039DEA48 = modelSceneShadow_calcViewMaskedLightShadow:
lis r3, modelSceneShadow_calcViewMaskedLightShadow@ha
addi r3, r3, modelSceneShadow_calcViewMaskedLightShadow@l
mtctr r3
lwz r3, 0x0C(r1)
bctrl ; bl modelSceneShadow_calcViewMaskedLightShadow

exit_SkipShadowCalcViewMasked:
lwz r4, 0x08(r1)
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x039AC9B0 = bla hook_skipShadowCalcViewMasked

; --------------------------------------------------------------------------------
; Patches below disable cascaded shadow map updating draw calls for the right eye

hook_shadowSetSizeLeftOnly:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
cmpwi r3, 1
beq exit_ShadowSetSizeLeftOnly

0x03B159F0 = shadowMap_setSize:
lis r3, shadowMap_setSize@ha
addi r3, r3, shadowMap_setSize@l
mtctr r3
lwz r3, 0x0C(r1)
bctrl ; bl shadowMap_setSize

exit_ShadowSetSizeLeftOnly:
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x039E02F8 = bla hook_shadowSetSizeLeftOnly

hook_shadowAllocBufferLeftOnly:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
cmpwi r3, 1
beq exit_ShadowAllocBufferLeftOnly

0x03B0ADC4 = shadowMap_allocDepthBuffer:
lis r3, shadowMap_allocDepthBuffer@ha
addi r3, r3, shadowMap_allocDepthBuffer@l
mtctr r3
lwz r3, 0x0C(r1)
bctrl ; bl shadowMap_allocDepthBuffer

exit_ShadowAllocBufferLeftOnly:
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x039E0304 = bla hook_shadowAllocBufferLeftOnly

hook_shadowDrawMapLeftOnly:
mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x0C(r1)
stw r4, 0x08(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
cmpwi r3, 1
beq exit_ShadowDrawMapLeftOnly

0x03B0ADF0 = depthShadow_drawShadowMap:
lis r3, depthShadow_drawShadowMap@ha
addi r3, r3, depthShadow_drawShadowMap@l
mtctr r3
lwz r3, 0x0C(r1)
bctrl ; bl depthShadow_drawShadowMap

exit_ShadowDrawMapLeftOnly:
lwz r4, 0x08(r1)
lwz r3, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x039E0310 = bla hook_shadowDrawMapLeftOnly
