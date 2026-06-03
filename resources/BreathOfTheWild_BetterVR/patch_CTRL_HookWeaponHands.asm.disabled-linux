[BetterVR_WeaponHands_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

0x020081AC = PlayerOrEnemy__dropWeapon:
0x033BCD9C = ActorWeapons__resetBaseProc:

dropPosition:
.float 0.0
.float 0.0
.float 0.0

changeWeaponMtx:
lwz r3, 0x58(r1) ; the actor we hackily put on the stack

mflr r0
stwu r1, -0x50(r1)
stw r0, 0x54(r1)

stw r3, 0x1C(r1)
mr r3, r31

stw r3, 0x08(r1)
stw r4, 0x0C(r1)
stw r5, 0x10(r1)
stw r6, 0x14(r1)
stw r7, 0x18(r1)
stw r8, 0x20(r1)
stw r9, 0x24(r1)
stw r10, 0x28(r1)
stw r11, 0x2C(r1)
stw r12, 0x30(r1)
stfs f1, 0x34(r1)
stw r13, 0x38(r1)

; get camera from global camera instance
lis r3, Camera__getLookAtCamera@ha
addi r3, r3, Camera__getLookAtCamera@l
mtctr r3
lis r3, Camera__sInstance@ha
lwz r3, Camera__sInstance@l(r3)
bctrl ; bl Camera__getLookAtCamera
mr. r10, r3 ; move camera to r10
beq noChangeWeaponMtx

; call C++ code to change the weapon mtx to the hand mtx
lwz r3, 0x1C(r1) ; the source actor
lwz r4, 0x18(r31) ; the char array of the weapon name
lwz r5, 0x10(r1) ; the target MTX
addi r6, r29, 0x34 ; the gsysModel->mtx, maybe used for the location?
addi r7, r31, 0x3C ; the mtx of the item supposedly
lwz r8, 0x0C(r1) ; the target actor
li r13, 0
bl import.coreinit.hook_ChangeWeaponMtx
stw r13, 0x3C(r1) ; store the hand idx

cmpwi r9, 0
beq noChangeWeaponMtx

lwz r3, 0x6C(r31)
li r5, 3
;stw r5, 0x6C(r31)


noChangeWeaponMtx:

; if drop weapon is set to 1, we will drop the weapon
cmpwi r11, 0
beq noWeaponDrop


dropWeapon:
lwz r3, 0x0C(r1)
lwz r3, 0xE8(r3)
lwz r3, 0x57C(r3)
mtctr r3
li r5, 0 ; spin-y drop caused by electricity
li r6, 0 ; also kinda spin-y
li r7, 0 ; this is some pointer?
li r8, 0 ; unsure
lis r4, dropPosition@ha
addi r4, r4, dropPosition@l
lwz r3, 0x0C(r1) ; r3 = Weapon*
bctrl ; call Weapon::m_174_dropPos
lwz r3, 0x1C(r1) ; r3 = Actor*
lwz r3, 0xE8(r3)
lwz r3, 0x314(r3)
mtctr r3
lwz r3, 0x1C(r1) ; r3 = Actor*
bctrl ; call PlayerOrEnemy::m_97_getWeapons
; r3 now contains the ActorWeapons*
lis r4, ActorWeapons__resetBaseProc@ha
addi r4, r4, ActorWeapons__resetBaseProc@l
mtctr r4
lwz r5, 0x3C(r1) ; r5 = hand idx
bctrl ; call ActorWeapons::resetBaseProc

noWeaponDrop:
lwz r3, 0x08(r1)
lwz r4, 0x0C(r1)
lwz r5, 0x10(r1)
lwz r6, 0x14(r1)
lwz r7, 0x18(r1)
lwz r8, 0x20(r1)
lwz r9, 0x24(r1)
lwz r10, 0x28(r1)
lwz r11, 0x2C(r1)
lwz r12, 0x30(r1)
lfs f1, 0x34(r1)
lwz r13, 0x38(r1)

lwz r0, 0x54(r1)
mtlr r0
addi r1, r1, 0x50

blr

; 0x03125438 = li r5, 3 ; this forces all weapons to be static

0x0312587C = bla changeWeaponMtx

; we want to preserve r7 since we need it later
;0x0312584C = lwz r10, 0x24(r29)
;0x03125854 = lwzx r10, r6, r10

; increase stack size
0x0312575C = stwu r1, -0x54(r1)
0x03125898 = addi r1, r1, 0x54
0x03125900 = addi r1, r1, 0x54
0x03125924 = addi r1, r1, 0x54

; store r7 on the stack
storeR7OnStack:
stw r3, 0x58(r1) ; store result of the function
mr. r7, r3
blr

0x0312577C = bla storeR7OnStack

; hook to check if a weapon should be dropped, and if yes, drop it
0x02D497C4 = PlayerInfo__getActor:
0x10463F38 = PlayerInfo__sInstance:

checkIfDropWeapon:
mflr r0
stwu r1, -0x20(r1)
stw r0, 0x24(r1)
stw r3, 0x1C(r1)
stw r4, 0x18(r1)
stw r5, 0x14(r1)

; lis r3, PlayerInfo__getActor@ha
; addi r3, r3, PlayerInfo__getActor@l
; mtctr r3
; lis r3, PlayerInfo__sInstance@ha
; lwz r3, PlayerInfo__sInstance@l(r3)
; bctrl ; bl PlayerInfo__getActor
; cmpwi r3, 0
; beq exit_checkIfDropWeapon

; check 

;li r3, 0
;bl import.coreinit.hook_DropEquipment

exit_checkIfDropWeapon:
lwz r5, 0x14(r1)
lwz r4, 0x18(r1)
lwz r3, 0x1C(r1)
lwz r0, 0x24(r1)
addi r1, r1, 0x20
mtlr r0
blr



; PlayerOrEnemy__dropWeapon
0x033BD09C = ActorWeapons__dropWeapon:

custom_PlayerOrEnemy__dropWeapon:
stwu r1, -0x20(r1)
stmw r26, 8(r1)
mflr r0
stw r0, 0x24(r1)

bl import.coreinit.hook_DropWeaponLogging

mr r31, r4
mr r26, r5
mr r27, r6
mr r28, r7
mr r30, r9
mr r29, r8
lwz r12, 0xE8(r3)
lwz r0, 0x314(r12)
mtctr r0
bctrl

mr r4, r31 ; idx
mr r5, r26 ; a3
mr r6, r27 ; a4
mr r7, r28 ; a5
mr r8, r29 ; a6
mr r9, r30 ; broke
bl ActorWeapons__dropWeapon
lmw r26, 8(r1)
lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

0x020081AC = ba custom_PlayerOrEnemy__dropWeapon

; =========================================

hook_changeWeaponScale:
stw r9, 0x27C(r31)

mflr r0
stwu r1, -0x10(r1)
stw r0, 0x14(r1)
stw r3, 0x08(r1)

bla import.coreinit.hook_SetPlayerWeaponScale

lwz r3, 0x08(r1)
lwz r0, 0x14(r1)
addi r1, r1, 0x10
mtlr r0
blr

0x024A92D4 = bla hook_changeWeaponScale