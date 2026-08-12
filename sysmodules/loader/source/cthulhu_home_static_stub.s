.section .rodata
.balign 4
.arm
.global cthulhuHomeStubStart
.global cthulhuHomeStubEnd
.global cthulhuFrameHook
.global cthulhuDisplayTransferHook
.global cthulhuKeysHeldHook
.global cthulhuKeysDownHook
.global cthulhuKeysUpHook
.global cthulhuInputDispatcherHook
.global cthulhuLayoutEventHook
.global cthulhuIconControllerInitHook
.global cthulhuIconRefreshObserveHook

cthulhuHomeStubStart:
    stmfd sp!, {r0-r3, r12, lr}
    ldr r1, markerAddress
    ldr r2, markerMagic
    str r2, [r1]
    ldr r2, abiVersion
    str r2, [r1, #4]
    ldr r2, [r1, #0x30]
    cmp r2, #0
    bne 9f
    stmfd sp!, {r4}
    ldr r0, linearAllocOperation
    mov r1, #0
    mov r2, #0
    mov r3, #0x20000
    mov r4, #3
    svc 0x01
    ldr r2, markerAddress
    str r0, [r2, #0x34]
    tst r0, #0x80000000
    bne 10f
    str r1, [r2, #0x30]
    mov r0, r1
    bl initializePanel
    @ initializePanel may clobber r0-r3 under the ARM ABI. Reload the channel
    @ pointer before publishing panel-ready instead of reusing stale r2.
    ldr r2, markerAddress
    mov r0, #1
    str r0, [r2, #0x3C]
10:
    ldmfd sp!, {r4}
9:
    ldmfd sp!, {r0-r3, r12, lr}
    ldr pc, entryTarget
entryTarget:   .word 0x00100024
markerAddress: .word 0x003827F0
markerMagic:   .word 0x43545337
abiVersion:    .word 0x00010000
linearAllocOperation: .word 0x00010003
panelWordCount: .word 0x00007080
panelBackground: .word 0x30303030

@ Build a 160x240 RGB8 OSD surface in the allocated linear buffer. The framebuffer
@ is column-major, so pixel(x,y) = base + x*720 + (239-y)*3.
initializePanel:
    stmfd sp!, {r4-r11, lr}
    mov r4, r0
    mov r1, r0
    ldr r2, panelBackground
    ldr r3, panelWordCount
17:
    str r2, [r1], #4
    subs r3, r3, #1
    bne 17b
    adr r5, panelGlyphs
    mov r6, #16
18:
    ldrb r1, [r5, #0]
    ldrb r2, [r5, #1]
    mov r7, #0
19:
    add r0, r5, #2
    ldrb r9, [r0, r7]
    mov r8, #0
20:
    mov r0, #0x10
    mov r0, r0, lsr r8
    tst r9, r0
    beq 21f
    add r0, r1, r8, lsl #1
    mov r3, #720
    mul r10, r0, r3
    add r0, r2, r7, lsl #1
    rsb r0, r0, #239
    add r0, r0, r0, lsl #1
    add r10, r10, r0
    add r10, r4, r10
    mov r11, #0xFF
    strb r11, [r10, #0]
    strb r11, [r10, #1]
    strb r11, [r10, #2]
    sub r0, r10, #3
    strb r11, [r0, #0]
    strb r11, [r0, #1]
    strb r11, [r0, #2]
    add r10, r10, #720
    strb r11, [r10, #0]
    strb r11, [r10, #1]
    strb r11, [r10, #2]
    sub r0, r10, #3
    strb r11, [r0, #0]
    strb r11, [r0, #1]
    strb r11, [r0, #2]
21:
    add r8, r8, #1
    cmp r8, #5
    blt 20b
    add r7, r7, #1
    cmp r7, #7
    blt 19b
    add r5, r5, #10
    subs r6, r6, #1
    bne 18b
    ldmfd sp!, {r4-r11, pc}

.balign 4
panelGlyphs:
    @ x, y, then seven 5-bit rows and one pad byte: CTHULU / SEARCH
    .byte 4,30, 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0
    .byte 16,30, 0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0
    .byte 28,30, 0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0
    .byte 40,30, 0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0
    .byte 52,30, 0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0
    .byte 64,30, 0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0
    .byte 4,60, 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E,0
    .byte 16,60, 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0
    .byte 28,60, 0x0E,0x11,0x11,0x1F,0x11,0x11,0x11,0
    .byte 40,60, 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0
    .byte 52,60, 0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0
    .byte 64,60, 0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0
    @ Visible payload version: V167
    .byte 16,90, 0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0
    .byte 28,90, 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0
    .byte 40,90, 0x0E,0x11,0x01,0x06,0x01,0x11,0x0E,0
    .byte 52,90, 0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0

cthulhuFrameHook:
    stmfd sp!, {r4-r6, r12, lr}
    sub sp, sp, #4
    ldr r12, frameTarget
    blx r12
    ldr r4, channelAddress
    ldr r5, [r4, #8]
    add r5, r5, #1
    str r5, [r4, #8]

    @ Rosalina publishes the layout-owner object and increments request at
    @ +0xCC. Rebuild the grid inside HOME at this frame boundary, then ack.
    ldr r5, [r4, #0xCC]
    ldr r6, [r4, #0xD4]
    cmp r5, r6
    beq 22f
    ldr r6, [r4, #0xD0]
    cmp r6, #0
    beq 22f
    stmfd sp!, {r0-r3}
    mov r0, r6
    ldr r12, layoutRebuildTarget
    blx r12
    ldr r0, [r4, #0xE0]
    cmp r0, #0
    moveq r0, r6
    mov r1, #1
    ldr r12, layoutPublishTarget
    blx r12
    str r0, [r4, #0xDC]
    ldr r0, [r4, #0x100]
    cmp r0, #0
    beq 24f
    ldr r12, iconRefreshTarget
    blx r12
    ldr r0, [r4, #0x104]
    add r0, r0, #1
    str r0, [r4, #0x104]
24:
    ldr r0, [r4, #0xE4]
    cmp r0, #0
    beq 23f
    mov r1, #0
    ldr r2, [r4, #0xEC]
    ldr r12, layoutEventTarget
    blx r12
    str r0, [r4, #0xF0]
    ldr r0, [r4, #0xF4]
    add r0, r0, #1
    str r0, [r4, #0xF4]
23:
    ldr r6, [r4, #0xD8]
    add r6, r6, #1
    str r6, [r4, #0xD8]
    str r5, [r4, #0xD4]
    ldmfd sp!, {r0-r3}
22:

    @ HOME maps HID shared memory here. Publish its current ring entry.
    ldr r5, hidSharedMemory
    ldr r6, [r5, #0x1C]
    str r6, [r4, #12]
    ldr r6, [r5, #0x10]
    and r6, r6, #7
    str r6, [r4, #24]
    add r12, r5, #0x28
    add r12, r12, r6, lsl #4
    ldr r5, [r12, #4]
    cmp r5, #0
    beq 1f
    str r5, [r4, #16]
    ldr r6, [r4, #28]
    add r6, r6, #1
    str r6, [r4, #28]
1:
    ldr r5, [r12, #8]
    cmp r5, #0
    strne r5, [r4, #20]
    @ Read-only client-0 GX queue scan. Locate commands by ID instead of slot,
    @ since HOME rotates the ring between frames. Preserve the original return
    @ registers while using r0-r3 for the bounded scan.
    stmfd sp!, {r0-r3}
    ldr r0, gspSharedMemory
    add r0, r0, #0x800
    ldr r6, [r0, #0]
    str r6, [r4, #0x80]
    add r0, r0, #0x20
    add r1, r4, #0x40
    add r2, r4, #0x60
    mov r3, #15
2:
    ldr r5, [r0]
    and r6, r5, #0xFF
    cmp r6, #2
    moveq r12, r1
    beq 3f
    cmp r6, #3
    moveq r12, r2
    bne 5f
3:
    mov r5, r0
    mov r6, #8
4:
    ldr lr, [r5], #4
    str lr, [r12], #4
    subs r6, r6, #1
    bne 4b
5:
    add r0, r0, #0x20
    subs r3, r3, #1
    bne 2b

    ldmfd sp!, {r0-r3}
    add sp, sp, #4
    ldmfd sp!, {r4-r6, r12, pc}
frameTarget:     .word 0x00102298
layoutRebuildTarget: .word 0x0013C680
layoutPublishTarget: .word 0x00146D10
layoutEventTarget: .word 0x001BA594
iconRefreshTarget: .word 0x001CA504

@ HOME's central navigation-event dispatcher. It receives a state mask in r0
@ and normally returns that same value after invoking registered callbacks.
cthulhuInputDispatcherHook:
    ldr r1, channelAddress
    ldr r2, [r1, #0x88]
    add r2, r2, #1
    str r2, [r1, #0x88]
    ldr r2, [r1, #32]
    cmp r2, #1
    bne 12f
    ldr r2, [r1, #0x8C]
    add r2, r2, #1
    str r2, [r1, #0x8C]
    bx lr
12:
    stmfd sp!, {r4-r10, lr}
    ldr pc, inputDispatcherContinue
inputDispatcherContinue: .word 0x001039E4

cthulhuLayoutEventHook:
    ldr r12, channelAddress
    str r0, [r12, #0xE4]
    str r1, [r12, #0xE8]
    str r2, [r12, #0xEC]
    ldr r12, layoutEventContinue
    cmp r2, #8
    bx r12
layoutEventContinue: .word 0x001BA598

@ Capture HOME's persistent icon controller and active page during its normal
@ initialization call, then tail-call the displaced native routine.
cthulhuIconControllerInitHook:
    ldr r12, channelAddress
    str r0, [r12, #0xE4]
    mov r2, #1
    str r2, [r12, #0xE8]
    str r1, [r12, #0xEC]
    ldr pc, iconControllerInitTarget
iconControllerInitTarget: .word 0x001B9ED4

@ Observe HOME's natural invocation of the confirmed 360-icon refresh routine.
@ Record its real object pointer, replay the displaced prologue, and continue.
cthulhuIconRefreshObserveHook:
    stmfd sp!, {r1, r12}
    ldr r12, channelAddress
    str r0, [r12, #0xF8]
    ldr r1, [r12, #0xFC]
    add r1, r1, #1
    str r1, [r12, #0xFC]
    ldmfd sp!, {r1, r12}
    stmfd sp!, {r4-r11, r12, lr}
    ldr pc, iconRefreshObserveContinue
iconRefreshObserveContinue: .word 0x001CA508

@ HOME's central button-query wrappers. When the overlay owns input, report no
@ buttons. Otherwise execute the displaced prologue and resume each function.
cthulhuKeysHeldHook:
    ldr r1, channelAddress
    ldr r2, [r1, #0x90]
    add r2, r2, #1
    str r2, [r1, #0x90]
    ldr r1, [r1, #32]
    cmp r1, #1
    moveq r0, #0
    bxeq lr
    stmfd sp!, {r4, lr}
    ldr pc, keysHeldContinue
keysHeldContinue: .word 0x001F6BCC

cthulhuKeysDownHook:
    ldr r1, channelAddress
    ldr r2, [r1, #0x90]
    add r2, r2, #1
    str r2, [r1, #0x90]
    ldr r1, [r1, #32]
    cmp r1, #1
    moveq r0, #0
    bxeq lr
    stmfd sp!, {r4, lr}
    ldr pc, keysDownContinue
keysDownContinue: .word 0x001F6C18

cthulhuKeysUpHook:
    ldr r1, channelAddress
    ldr r2, [r1, #0x90]
    add r2, r2, #1
    str r2, [r1, #0x90]
    ldr r1, [r1, #32]
    cmp r1, #1
    moveq r0, #0
    bxeq lr
    stmfd sp!, {r4, lr}
    ldr pc, keysUpContinue
keysUpContinue: .word 0x001F6DC8

@ Replaces the sole BL to HOME's DisplayTransfer wrapper target. Preserve the
@ original stack arguments by temporarily restoring sp before the native call,
@ then append the OSD only after a 400x240 top-screen transfer.
cthulhuDisplayTransferHook:
    stmfd sp!, {r4-r7, lr}
    mov r5, r3
    sub sp, sp, #12
    ldr r12, [sp, #32]
    str r12, [sp, #0]
    mov r7, r12
    ldr r12, [sp, #36]
    str r12, [sp, #4]
    ldr r12, [sp, #40]
    str r12, [sp, #8]
    ldr r12, displayTransferTarget
    blx r12
    add sp, sp, #12
    mov r6, r0
    ldr r4, channelAddress
    ldr r0, [r4, #0x84]
    add r0, r0, #1
    str r0, [r4, #0x84]
    str r5, [r4, #0xB0]
    str r7, [r4, #0xB4]
    ldr r0, gspSharedMemory
    ldr r1, [r0, #0x208]
    str r1, [r4, #0xBC]
    cmp r5, r1
    beq 13f
    ldr r1, [r0, #0x20C]
    str r1, [r4, #0xC0]
    cmp r5, r1
    beq 13f
    ldr r1, [r0, #0x224]
    str r1, [r4, #0xC4]
    cmp r5, r1
    beq 13f
    ldr r1, [r0, #0x228]
    str r1, [r4, #0xC8]
    cmp r5, r1
    bne 16f
13:
    ldr r0, [r4, #0xB8]
    add r0, r0, #1
    str r0, [r4, #0xB8]
    ldr r0, [r4, #32]
    cmp r0, #1
    bne 16f
    sub sp, sp, #0x20
    ldr r5, dmaCommandHeader
    str r5, [sp, #0]
    ldr r5, [r4, #0x30]
    str r5, [sp, #4]
    ldr r5, [r4, #0xB0]
    str r5, [sp, #8]
    ldr r5, panelByteCount
    str r5, [sp, #12]
    mov r5, #0
    str r5, [sp, #16]
    str r5, [sp, #20]
    str r5, [sp, #24]
    mov r5, #1
    str r5, [sp, #28]
    ldr r12, graphicsContextGetter
    blx r12
    add r0, r0, #0x58
    mov r1, sp
    ldr r12, queueSubmit
    blx r12
    str r0, [r4, #0x24]
    add sp, sp, #0x20
    ldr r0, [r4, #0x2C]
    add r0, r0, #1
    str r0, [r4, #0x2C]
16:
    mov r0, r6
    ldmfd sp!, {r4-r7, pc}
displayTransferTarget: .word 0x0014BC78
channelAddress:  .word 0x003827F0
hidSharedMemory: .word 0x10000000
gspSharedMemory: .word 0x10002000
graphicsContextGetter: .word 0x002225F0
queueSubmit:           .word 0x0021820C
dmaCommandHeader:      .word 0x00000100
panelByteCount:        .word 0x0001C200
.balign 4
overlayFillCommand:
    .word 0x01000102
    .word 0x1F4E1DE0
    .word 0x0000FFFF
    .word 0x1F4E5620
    .word 0x1F4A95E0
    .word 0x0000FFFF
    .word 0x1F4ACE20
    .word 0x02010201

cthulhuHomeStubEnd:
