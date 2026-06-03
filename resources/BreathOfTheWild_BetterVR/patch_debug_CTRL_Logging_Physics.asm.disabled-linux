[BetterVR_ControlsLoggingPhysics_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

LogSetRigidBodyVelocity:
stwu r1, -0x10(r1)
mflr r0
stw r0, 0x14(r1)
stw r3, 0x08(r1)
stw r4, 0x0C(r1)

bla import.coreinit.hook_SetRigidBodyVelocity

lwz r3, 0x08(r1)
lwz r4, 0x0C(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
lis r12, ContinueAfterLogSetRigidBodyVelocity@ha
addi r12, r12, ContinueAfterLogSetRigidBodyVelocity@l
mtctr r12
bctr

; hooks entity transform updates
0x0348970C = ba LogSetRigidBodyVelocity
0x03485F68 = mflr r0
0x03486CD4 = ba import.coreinit.hook_SetRigidBodyTransform
0x03486F4C = ba import.coreinit.hook_SetRigidBodyScale
;0x0380E8F0 = ba import.coreinit.hook_RemoveRagdollControllerFromWorld
;0x0380F1D0 = ba import.coreinit.hook_SetRagdollControllerTransform
;0x0380F258 = ba import.coreinit.hook_SetRagdollControllerScale
0x03486D80 = ba import.coreinit.hook_SetRigidBodyPosition
0x03489A80 = ba import.coreinit.hook_SetRigidBodyPositionAndRotation
0x0348AB20 = mflr r0

; temporary sanity check: intercept Player::m_84_setMtx just before Actor::m_84_setMtx
; IDA: 0x02D66B58 = mr r4, r29
;0x02D66B58 = bla import.coreinit.hook_TestPlayerSetMtxTeleport

; hooks arrow shooting parameters (and other dynamically loaded bools/values inside AI Actions)
0x030EC840 = ba import.coreinit.hook_LoadDynamicVec3
0x030EC8C0 = bla import.coreinit.hook_LoadDynamicBool

; swap TargetPos and IsShootByPlayer to be able to mutate the TargetPos value AFTER we know that the arrow is shot by the player

; .origin = 0x02705AFC
; 
; lis r6, str_IsShootByPlayer@ha
; addi r6, r6, str_IsShootByPlayer@l
; lwz r5, 0x0E8(r29)
; stw r27, 0x014(r1)
; stw r6, 0x010(r1)
; lwz r8, 0x4EC(r5)
; mtctr r8
; mr r3, r29
; bctrl
; mr r4, r3
; li r6, -1
; addi r5, r1, 0x010
; addi r3, r1, 0x158
; bl ksys__act__ai__InlineParamPack__addBool
; 
; lis r4, str_TargetPos@ha
; addi r4, r4, str_TargetPos@l
; li r6, -1
; addi r5, r1, 0x010
; stw r4, 0x010(r1)
; addi r4, r1, 0x88
; addi r3, r1, 0x158
; stw r27, 0x14(r1)
; bl ksys__act__ai__InlineParamPack__addVec3
; 
; .origin = codecave

0x1011A7A0 = str_IsShootByPlayer:
0x1011A7F4 = str_TargetPos:
0x030EC860 = ksys__act__ai__InlineParamPack__addBool:
0x030EC7DC = ksys__act__ai__InlineParamPack__addVec3:

0x0344AC6C = ApplyPlayerVelocityInternal:
