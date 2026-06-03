[BetterVR_Rumble_V208]
moduleMatches = 0x6267BFD0

.origin = codecave

hook_VPADControlMotor:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)

bla import.coreinit.hook_XRRumble_VPADControlMotor

bla import.vpad.VPADControlMotor

lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

0x030D90BC = bla hook_VPADControlMotor
0x030D9170 = bla hook_VPADControlMotor


hook_VPADStopMotor:
stwu r1, -0x20(r1)
mflr r0
stw r0, 0x24(r1)

bla import.coreinit.hook_XRRumble_VPADStopMotor

bla import.vpad.VPADStopMotor

lwz r0, 0x24(r1)
mtlr r0
addi r1, r1, 0x20
blr

0x030D91F0 = bla hook_VPADStopMotor
