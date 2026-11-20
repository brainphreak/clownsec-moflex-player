@ ARMv6 (ARM11) motion compensation for MobiClip, any source alignment, 4- and 8-pixel-wide blocks.
@ 16-wide blocks are done as two 8-wide halves (same alignment, everything stays in registers).
@
@   void mcN_mM(uint8_t *dst, const uint8_t *src, int h, int dls, int sls)     N = words (1|2), M = method
@     m0: copy   m1: (s[x]>>1)+(s[x+1]>>1)   m2: (s[x]>>1)+(s[x+sls]>>1)   m3: m1 of two rows, then m2-averaged
@   dst, dls, sls: multiples of 4.   Bit-exact with mc_fast_t() in mobiclip.c.
@
@ Unaligned source: the words covering a row are loaded aligned, and each output word is
@   w0 = (a[i] >> k) | (a[i+1] << (32-k))         k = 8 * (src & 3)
@ using REGISTER shifts, where LSL by 32 gives 0 and LSR by 0 is a no-op -- so k = 0 needs no special
@ case. The +1-shifted word (half-pel) is w1 = (w0 >> 8) | ((a[i+1] >> k) << 24).
@ Truncating average (a>>1)+(b>>1) per byte: ((a>>1) & 0x7F7F7F7F) + ((b>>1) & 0x7F7F7F7F).
@
@ Registers: r0 dst  r1 aligned src  r2 rows  r3 dls  r4 sls  r5 k  r6 32-k  r7 0x7F7F7F7F
@            r8 r9 previous row (m2/m3)   r10 r11 r12 lr scratch
    .arch armv6k
    .arm
    .text

.macro PROLOGUE
    push    {r4-r11, lr}
    ldr     r4, [sp, #36]           @ sls
    and     r5, r1, #3
    bic     r1, r1, #3
    lsl     r5, r5, #3              @ k
    rsb     r6, r5, #32             @ 32 - k
    ldr     r7, =0x7F7F7F7F
.endm
.macro EPILOGUE
    pop     {r4-r11, pc}
.endm
@ avg into d: d = ((x>>1)&M) + ((y>>1)&M); x, y preserved unless aliased; t scratch
.macro AVG d, x, y, t
    and     \t, r7, \y, lsr #1
    and     \d, r7, \x, lsr #1
    add     \d, \d, \t
.endm

@ ---------------- 1 word (4 px) ----------------
    .global mc1_m0
    .type mc1_m0, %function
mc1_m0:
    PROLOGUE
1:  ldm     r1, {r10, r11}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6
    str     r10, [r0], r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .global mc1_m1
    .type mc1_m1, %function
mc1_m1:
    PROLOGUE
1:  ldm     r1, {r10, r11}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6   @ w0
    lsr     r11, r11, r5            @ a1 >> k
    lsr     r12, r10, #8
    orr     r12, r12, r11, lsl #24  @ w1
    AVG     r10, r10, r12, lr
    str     r10, [r0], r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .global mc1_m2
    .type mc1_m2, %function
mc1_m2:
    PROLOGUE
    ldm     r1, {r10, r11}
    add     r1, r1, r4
    lsr     r8, r10, r5
    orr     r8, r8, r11, lsl r6     @ prev = w0(row 0)
1:  ldm     r1, {r10, r11}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6   @ cur
    AVG     r12, r8, r10, lr
    mov     r8, r10
    str     r12, [r0], r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .global mc1_m3
    .type mc1_m3, %function
mc1_m3:
    PROLOGUE
    ldm     r1, {r10, r11}
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6
    lsr     r11, r11, r5
    lsr     r12, r10, #8
    orr     r12, r12, r11, lsl #24
    AVG     r8, r10, r12, lr        @ prev = havg(row 0)
1:  ldm     r1, {r10, r11}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6
    lsr     r11, r11, r5
    lsr     r12, r10, #8
    orr     r12, r12, r11, lsl #24
    AVG     r10, r10, r12, lr       @ cur = havg(row)
    AVG     r12, r8, r10, lr
    mov     r8, r10
    str     r12, [r0], r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

@ ---------------- 2 words (8 px) ----------------
    .global mc2_m0
    .type mc2_m0, %function
mc2_m0:
    PROLOGUE
1:  ldm     r1, {r10, r11, r12}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6   @ out0
    lsr     r11, r11, r5
    orr     r11, r11, r12, lsl r6   @ out1
    stm     r0, {r10, r11}
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .global mc2_m1
    .type mc2_m1, %function
mc2_m1:
    PROLOGUE
1:  ldm     r1, {r10, r11, r12}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6   @ w0_0
    lsr     r11, r11, r5            @ t1 = a1 >> k
    lsr     lr, r10, #8
    orr     lr, lr, r11, lsl #24    @ w1_0
    and     lr, r7, lr, lsr #1
    and     r10, r7, r10, lsr #1
    add     r10, r10, lr            @ out0
    orr     r11, r11, r12, lsl r6   @ w0_1
    lsr     r12, r12, r5            @ t2
    lsr     lr, r11, #8
    orr     lr, lr, r12, lsl #24    @ w1_1
    and     lr, r7, lr, lsr #1
    and     r11, r7, r11, lsr #1
    add     r11, r11, lr            @ out1
    stm     r0, {r10, r11}
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .global mc2_m2
    .type mc2_m2, %function
mc2_m2:
    PROLOGUE
    ldm     r1, {r10, r11, r12}
    add     r1, r1, r4
    lsr     r8, r10, r5
    orr     r8, r8, r11, lsl r6     @ prev0
    lsr     r9, r11, r5
    orr     r9, r9, r12, lsl r6     @ prev1
1:  ldm     r1, {r10, r11, r12}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6   @ cur0
    lsr     r11, r11, r5
    orr     r11, r11, r12, lsl r6   @ cur1
    and     lr, r7, r8, lsr #1
    mov     r8, r10
    and     r10, r7, r10, lsr #1
    add     r10, r10, lr            @ out0
    and     lr, r7, r9, lsr #1
    mov     r9, r11
    and     r11, r7, r11, lsr #1
    add     r11, r11, lr            @ out1
    stm     r0, {r10, r11}
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

@ one row of the 8-px horizontal average: a0..a2 in r10-r12 -> r10 = havg0, r11 = havg1 (lr scratch)
.macro HAVG2
    lsr     r10, r10, r5
    orr     r10, r10, r11, lsl r6
    lsr     r11, r11, r5
    lsr     lr, r10, #8
    orr     lr, lr, r11, lsl #24
    and     lr, r7, lr, lsr #1
    and     r10, r7, r10, lsr #1
    add     r10, r10, lr
    orr     r11, r11, r12, lsl r6
    lsr     r12, r12, r5
    lsr     lr, r11, #8
    orr     lr, lr, r12, lsl #24
    and     lr, r7, lr, lsr #1
    and     r11, r7, r11, lsr #1
    add     r11, r11, lr
.endm

    .global mc2_m3
    .type mc2_m3, %function
mc2_m3:
    PROLOGUE
    ldm     r1, {r10, r11, r12}
    add     r1, r1, r4
    HAVG2
    mov     r8, r10
    mov     r9, r11                 @ prev = havg(row 0)
1:  ldm     r1, {r10, r11, r12}
    pld     [r1, r4, lsl #1]
    add     r1, r1, r4
    HAVG2                           @ cur in r10, r11
    and     lr, r7, r8, lsr #1
    mov     r8, r10
    and     r10, r7, r10, lsr #1
    add     r10, r10, lr            @ out0
    and     lr, r7, r9, lsr #1
    mov     r9, r11
    and     r11, r7, r11, lsr #1
    add     r11, r11, lr            @ out1
    stm     r0, {r10, r11}
    add     r0, r0, r3
    subs    r2, r2, #1
    bne     1b
    EPILOGUE

    .ltorg
