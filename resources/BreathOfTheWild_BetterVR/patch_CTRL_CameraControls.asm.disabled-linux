[BetterVR_CameraControls_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

; disable camera recentering using the shield button
0x02B96E10 = li r3, 0

; ==================================================================================
; For gameplay and culling reasons, we update the camera position earlier then just before rendering (see GetRenderCamera hook)
; GetRenderCamera() is still required since this function is only ran once per each pair of eyes being rendered.
; GetRenderCamera() discards the position and eyes that this function sets, since it can't easily apply only the differential.
; This function also lacks the ability to modify the up vector, so roll is not possible here, but its sufficient for gameplay.
updateCameraPositionAndTarget:
; repeat instructions from either branches
lfs f0, 0xEC0(r31)
stfs f0, 0x5CC(r31)
lfs f13, 0xEB8(r31)
stfs f13, 0x5C4(r31)

; function prologue
mflr r0
stwu r1, -0x58(r1)
stw r0, 0x5C(r1)
stw r3, 0x54(r1)

lis r3, currentEyeSide@ha
lwz r3, currentEyeSide@l(r3)
bl import.coreinit.hook_UpdateCameraForGameplay

exit_updateCameraPositionAndTarget:
; function epilogue
lwz r3, 0x54(r1)
lwz r0, 0x5C(r1)
mtlr r0
addi r1, r1, 0x58

addi r3, r31, 0xE78
blr


0x02C054FC = bla updateCameraPositionAndTarget
0x02C05590 = bla updateCameraPositionAndTarget


; ==================================================================================

adjustGameplayCameraPivot:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)

lis r12, 0x02B9
ori r12, r12, 0x7B28
mtctr r12
bctrl

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
bl import.coreinit.hook_AdjustGameplayCameraPivot

lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

adjustGameplayCameraPivot_InsideCameraPivotCalc:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)

lis r12, 0x02B9
ori r12, r12, 0x7834
mtctr r12
bctrl

lwz r3, 0x1C(r1)
lwz r4, 0x18(r1)
lwz r5, 0x14(r1)
bl import.coreinit.hook_AdjustGameplayCameraPivot

lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

0x02B97C34 = bla adjustGameplayCameraPivot_InsideCameraPivotCalc
0x02B99AB4 = bla adjustGameplayCameraPivot
0x02B9CCB8 = bla adjustGameplayCameraPivot


; ==================================================================================

cameraModePtr:
.int 0

storeCameraModePtr:
mr r3, r31

mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)

lis r4, cameraModePtr@ha
stw r3, cameraModePtr@l(r4)

lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr

;; store CameraFinder (camera controls. Fixes first-person mode)
;;0x02BCE5DC = bla storeCameraModePtr
;; store CameraKeep (no right-stick controls at all! Might not follow player?)
;;0x02BD55AC = bla storeCameraModePtr
;; store CameraTail (seems to fix pivot anchor issues and forward looking camera?!)
;0x02BEB244 = bla storeCameraModePtr
;; store CameraRevolve
;;0x02BE443C = bla storeCameraModePtr
;; store CameraAbyss (prevents all rotational camera, but follows player)
;;0x02B8E858 = bla storeCameraModePtr
; store CameraChase (normal third-person camera)
0x02B966A4 = bla storeCameraModePtr

useCameraFinder:
mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)

; r3 is modified by the hook, if needed
lis r4, cameraModePtr@ha
lwz r4, cameraModePtr@l(r4)
lwz r5, 0x0C(r3) ; load vtable pointer from current camera mode

bl import.coreinit.hook_ReplaceCameraMode

lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
;lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0

mr r31, r3
blr

0x02B8FCA4 = bla useCameraFinder


; ==================================================================================
; disables CameraChase's atMoveOffset
; also prevents camera from slowly drifting panning towards where Link is walking
0x02B9D164 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D184 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D1A4 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D1C4 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D1E4 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D204 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D224 = bla import.coreinit.hook_OverwriteFloatParam

0x02B9D244 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D264 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D284 = bla import.coreinit.hook_OverwriteFloatParam

; prevent camera from connecting to things
0x02B9D2E4 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D304 = bla import.coreinit.hook_OverwriteFloatParam
0x02B9D324 = bla import.coreinit.hook_OverwriteFloatParam


; workaround for ladder climbing issue
; Always sets the ladder mode to 4 which allows pressing A to jump up ladders
; Sets the ladder mode to 1 when player is moving the stick downwards to allow sliding down ladders
0x02D69E04 = ba import.coreinit.hook_FixLadder

0x02D07CE8 = ba import.coreinit.hook_PlayerLadderFix
