@ Hand-written ARMv6 (ARM11) MobiClip coefficient-entropy loop.
@ 1:1 translation of the host-verified C loop (mobi_opt 0x200) in add_coefficients_impl:
@   - big-endian bit reader held in registers (cache r1, bit-index r2), lazy refill
@   - flat 12-bit run/level VLC (VLCElem{int16 sym@0, int16 len@2}, max_depth 1 -> one lookup)
@   - checked reader: every skip clamps index to size_in_bits_plus8 (FFMIN)
@   int mobi_entropy_asm(MobiEntropyCtx *c);   returns 0 ok, 1 on pos-overflow
@ ctx offsets: buffer 0, index 4, sib 8, sib8 12, rltab 16, rres 20, qtab 24, ztab 28,
@              mat 32, size 36, rowmask 40, ac 44
    .arch armv6k
    .arm
    .text
    .global mobi_entropy_asm
    .type   mobi_entropy_asm, %function

@ r0 ctx | r1 cache | r2 bidx | r3 buf | r4 rltab | r5 rres | r6 pos
@ r7 sib8 | r8 sib | r9 level | r10 run | r11 last | r12/lr scratch

@ cache = REV(RB32(buf + bidx>>3)) << (bidx&7)   -- refill, clobbers r12,lr
    .macro UPDATE
    lsr     r12, r2, #3
    ldr     r12, [r3, r12]
    rev     r12, r12
    and     lr,  r2, #7
    lsl     r1,  r12, lr
    .endm

@ skip \n bits (immediate): cache<<=n; bidx = min(sib8, bidx+n)
    .macro SKIPI n
    lsl     r1,  r1, #\n
    add     r2,  r2, #\n
    cmp     r2,  r7
    movgt   r2,  r7
    .endm

@ read \n unsigned bits (no refill) into \dst, then skip
    .macro RDU dst, n
    lsr     \dst, r1, #(32-\n)
    SKIPI   \n
    .endm

@ read one run/level VLC (no refill): sets r9=level, r10=run, r11=last, skips len
    .macro RDVLC
    lsr     r12, r1, #20               @ top 12 bits = table index
    ldr     r12, [r4, r12, lsl #2]     @ VLCElem: sym(lo16) | len(hi16)
    sxth    lr,  r12                   @ n = (int16)sym
    asr     r12, r12, #16              @ len = (int16)len   (positive, <=12)
    lsl     r1,  r1, r12               @ SKIP(len): cache <<= len
    add     r2,  r2, r12
    cmp     r2,  r7
    movgt   r2,  r7
    and     r9,  lr, #0x1F             @ level = n & 0x1F
    mov     r10, lr, lsr #5
    and     r10, r10, #0x3F            @ run   = (n>>5) & 0x3F
    mov     r11, lr, lsr #11           @ last  = (n>>11)   (n is 12-bit -> 0/1)
    .endm

mobi_entropy_asm:
    push    {r4-r11, lr}
    ldr     r3,  [r0, #0]              @ buffer
    ldr     r2,  [r0, #4]              @ index
    ldr     r8,  [r0, #8]              @ size_in_bits
    ldr     r7,  [r0, #12]             @ size_in_bits_plus8
    ldr     r4,  [r0, #16]             @ rltab
    ldr     r5,  [r0, #20]             @ rres
    mov     r6,  #0                    @ pos = 0

.Lloop:
    subs    r12, r8, r2                @ BITS_LEFT = size_in_bits - index
    ble     .Ldone                     @ <=0 -> normal end

    UPDATE                             @ >=25 valid bits
    RDVLC                              @ VLC (<=12) -> level/run/last ; >=13 left

    cmp     r9,  #0                    @ if (level)
    beq     .Llevel0
    RDU     r12, 1                     @   sign bit
    cmp     r12, #0
    rsbne   r9,  r9, #0                @   level = -level
    b       .Lplace

.Llevel0:
    RDU     r12, 1                     @ b1
    cmp     r12, #0
    bne     .Lb1
    @ branch 2: level += rres[(last?64:0)+run]
    UPDATE
    RDVLC
    cmp     r11, #0
    movne   lr,  #64
    moveq   lr,  #0
    add     lr,  lr, r10               @ (last?64:0)+run
    ldrb    lr,  [r5, lr]
    add     r9,  r9, lr                @ level +=
    RDU     r12, 1
    cmp     r12, #0
    rsbne   r9,  r9, #0
    b       .Lplace

.Lb1:
    RDU     r12, 1                     @ b2
    cmp     r12, #0
    bne     .Lescape
    @ branch 3: run += rres[128+(last?64:0)+level]
    UPDATE
    RDVLC
    cmp     r11, #0
    movne   lr,  #64
    moveq   lr,  #0
    add     lr,  lr, #128
    add     lr,  lr, r9                @ 128+(last?64:0)+level
    ldrb    lr,  [r5, lr]
    add     r10, r10, lr               @ run +=
    RDU     r12, 1
    cmp     r12, #0
    rsbne   r9,  r9, #0
    b       .Lplace

.Lescape:
    @ branch 4: last=1b, run=6b, level=sbits(12)
    UPDATE
    RDU     r11, 1                     @ last
    RDU     r10, 6                     @ run
    asr     r9,  r1, #20               @ level = signed 12-bit
    SKIPI   12

.Lplace:
    add     r6,  r6, r10               @ pos += run
    ldr     r12, [r0, #36]             @ size
    mul     lr,  r12, r12              @ size*size
    cmp     r6,  lr
    bge     .Lerr                      @ pos >= size*size -> error

    ldr     lr,  [r0, #24]             @ qtab
    ldr     lr,  [lr, r6, lsl #2]      @ qval = qtab[pos]
    mul     lr,  r9,  lr               @ product = qval * level  (low 32 bits)
    ldr     r12, [r0, #28]             @ ztab
    ldrb    r12, [r12, r6]             @ zt = ztab[pos]
    ldr     r9,  [r0, #32]             @ mat base (level no longer needed)
    str     lr,  [r9, r12, lsl #2]     @ mat[zt] = product

    ldr     lr,  [r0, #36]             @ size
    mov     lr,  lr, lsr #2            @ 1 or 2
    add     lr,  lr, #1                @ log2(size) = 2 or 3
    mov     r9,  r12, lsr lr           @ row = zt >> log2(size)
    mov     lr,  #1
    lsl     r9,  lr, r9                @ 1 << row
    ldr     lr,  [r0, #40]             @ rowmask
    orr     lr,  lr, r9
    str     lr,  [r0, #40]

    cmp     r12, #0                    @ if (zt) ac++
    beq     .Lnoac
    ldr     lr,  [r0, #44]
    add     lr,  lr, #1
    str     lr,  [r0, #44]
.Lnoac:
    cmp     r11, #0                    @ if (last) break
    bne     .Ldone
    add     r6,  r6, #1                @ for-loop pos++
    b       .Lloop

.Ldone:
    str     r2,  [r0, #4]              @ write back index
    mov     r0,  #0
    pop     {r4-r11, pc}

.Lerr:
    str     r2,  [r0, #4]
    mov     r0,  #1
    pop     {r4-r11, pc}

    .size   mobi_entropy_asm, . - mobi_entropy_asm
