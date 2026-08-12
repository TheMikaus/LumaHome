.section .text
.balign 4
.arm
.global cthulhuPostBootTrampolineStart
.global cthulhuPostBootResumeLiteral
.global cthulhuPostBootTrampolineEnd
cthulhuPostBootTrampolineStart:
    stmfd sp!, {r0-r12, lr}
    mrs r0, cpsr
    stmfd sp!, {r0}
    ldr r5, cthulhuPostBootPluginEntry
    blx r5
    ldmfd sp!, {r0}
    msr cpsr, r0
    ldmfd sp!, {r0-r12, lr}
    ldr pc, cthulhuPostBootResumeLiteral
cthulhuPostBootPluginEntry:
    .word 0x07000100
cthulhuPostBootResumeLiteral:
    .word 0
cthulhuPostBootTrampolineEnd:
