@ Hand-written ARMv6 (ARM11) half-pel motion compensation for MobiClip.
@ Uses UHADD8 (parallel per-byte (a+b)>>1) corrected to mobiclip's truncated (a>>1)+(b>>1)
@ via  - ((a&b) & 0x01010101).  Reads the reference frame with ALIGNED word loads only
@ (the sub-word neighbor is built with a shift, never an unaligned load).
@ Requires: src 4-aligned, width a multiple of 4, width>=4.  C handles the rest.
@   void mc_havg_a(u8*dst, const u8*src, int w, int h, int ds, int ss)   dst=(s[x]>>1)+(s[x+1]>>1)
@   void mc_vavg_a(...)                                                   dst=(s[x]>>1)+(s[x+ss]>>1)
@   void mc_diag_a(...)                          dst=(((s[x]>>1)+(s[x+1]>>1))>>1)+(((s[x+ss]..)>>1)
    .arch armv6k
    .arm
    .text

@ ------- horizontal half-pel -------
    .global mc_havg_a
    .type mc_havg_a, %function
mc_havg_a:
    push    {r4-r11, lr}
    ldr     r12, [sp, #36]          @ ds
    ldr     lr,  [sp, #40]          @ ss
    ldr     r11, =0x01010101
.Lh_row:
    mov     r4, r0                  @ dst cursor
    mov     r5, r1                  @ src cursor
    subs    r6, r2, #4              @ group counter (w-4, step -4, run while >=0)
    ldr     r7, [r5], #4            @ w_cur
.Lh_col:
    ldr     r8, [r5], #4            @ w_next
    lsr     r9, r7, #8
    orr     r9, r9, r8, lsl #24     @ B = neighbor word (src[x+1..x+4])
    uhadd8  r10, r7, r9
    and     r9, r7, r9
    and     r9, r9, r11
    sub     r10, r10, r9
    str     r10, [r4], #4
    mov     r7, r8
    subs    r6, r6, #4
    bge     .Lh_col
    add     r0, r0, r12             @ dst += ds
    add     r1, r1, lr              @ src += ss
    subs    r3, r3, #1
    bgt     .Lh_row
    pop     {r4-r11, pc}

@ ------- vertical half-pel -------
    .global mc_vavg_a
    .type mc_vavg_a, %function
mc_vavg_a:
    push    {r4-r11, lr}
    ldr     r12, [sp, #36]          @ ds
    ldr     lr,  [sp, #40]          @ ss
    ldr     r11, =0x01010101
.Lv_row:
    mov     r4, r0
    mov     r5, r1
    mov     r6, r2                  @ w
.Lv_col:
    ldr     r7, [r5]                @ A = src[x..]
    ldr     r8, [r5, lr]            @ B = src[x+ss..]
    uhadd8  r10, r7, r8
    and     r9, r7, r8
    and     r9, r9, r11
    sub     r10, r10, r9
    str     r10, [r4], #4
    add     r5, r5, #4
    subs    r6, r6, #4
    bgt     .Lv_col
    add     r0, r0, r12
    add     r1, r1, lr
    subs    r3, r3, #1
    bgt     .Lv_row
    pop     {r4-r11, pc}

@ ------- diagonal (bilinear) half-pel -------
    .global mc_diag_a
    .type mc_diag_a, %function
mc_diag_a:
    push    {r4-r11, lr}
    ldr     r12, [sp, #36]          @ ds
    ldr     lr,  [sp, #40]          @ ss
    ldr     r11, =0x01010101
    sub     r4, r12, r2             @ dadj = ds - w
    sub     r5, lr, r2              @ sadj = ss - w
.Ld_row:
    mov     r6, r2                  @ w
.Ld_col:
    ldr     r7, [r1]                @ topA
    ldr     r8, [r1, #4]            @ topnext
    lsr     r9, r7, #8
    orr     r9, r9, r8, lsl #24     @ topB
    uhadd8  r8, r7, r9
    and     r7, r7, r9
    and     r7, r7, r11
    sub     r7, r8, r7              @ top
    ldr     r8, [r1, lr]            @ botA
    add     r10, r1, lr
    ldr     r9, [r10, #4]           @ botnext
    lsr     r10, r8, #8
    orr     r10, r10, r9, lsl #24   @ botB
    uhadd8  r9, r8, r10
    and     r8, r8, r10
    and     r8, r8, r11
    sub     r8, r9, r8              @ bot
    uhadd8  r9, r7, r8
    and     r7, r7, r8
    and     r7, r7, r11
    sub     r7, r9, r7              @ out
    str     r7, [r0], #4
    add     r1, r1, #4
    subs    r6, r6, #4
    bgt     .Ld_col
    add     r0, r0, r4              @ dst += (ds - w)
    add     r1, r1, r5              @ src += (ss - w)
    subs    r3, r3, #1
    bgt     .Ld_row
    pop     {r4-r11, pc}
    .ltorg
