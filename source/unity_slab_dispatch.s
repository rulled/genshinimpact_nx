/* Exact-client bridge for guest RVA 0x44C3A10.
 *
 * The replaced selector was not a call site, so preserve every caller-saved
 * integer/SIMD register around the broker call.  Once the selected 8 MiB chunk
 * is physically resident, reproduce the original global load, index mask and
 * flag-setting add before resuming at RVA 0x44C3A20. */

    .section .text.genshin_unity_slab_activate_dispatch,"ax",%progbits
    .p2align 2
    .global genshin_unity_slab_activate_dispatch
    .hidden genshin_unity_slab_activate_dispatch
    .type genshin_unity_slab_activate_dispatch, %function
genshin_unity_slab_activate_dispatch:
    sub sp, sp, #0x2b0
    stp x0,  x1,  [sp, #0x00]
    stp x2,  x3,  [sp, #0x10]
    stp x4,  x5,  [sp, #0x20]
    stp x6,  x7,  [sp, #0x30]
    stp x8,  x9,  [sp, #0x40]
    stp x10, x11, [sp, #0x50]
    stp x12, x13, [sp, #0x60]
    stp x14, x15, [sp, #0x70]
    stp x16, x17, [sp, #0x80]
    stp x18, x30, [sp, #0x90]
    stp q0,  q1,  [sp, #0xa0]
    stp q2,  q3,  [sp, #0xc0]
    stp q4,  q5,  [sp, #0xe0]
    stp q6,  q7,  [sp, #0x100]
    stp q8,  q9,  [sp, #0x120]
    stp q10, q11, [sp, #0x140]
    stp q12, q13, [sp, #0x160]
    stp q14, q15, [sp, #0x180]
    stp q16, q17, [sp, #0x1a0]
    stp q18, q19, [sp, #0x1c0]
    stp q20, q21, [sp, #0x1e0]
    stp q22, q23, [sp, #0x200]
    stp q24, q25, [sp, #0x220]
    stp q26, q27, [sp, #0x240]
    stp q28, q29, [sp, #0x260]
    stp q30, q31, [sp, #0x280]
    mrs x10, fpcr
    mrs x11, fpsr
    str x10, [sp, #0x2a0]
    str x11, [sp, #0x2a8]

    adrp x8, genshin_unity_slab_aligned_slot
    add  x8, x8, :lo12:genshin_unity_slab_aligned_slot
    ldr  x8, [x8]
    ldr  x8, [x8]
    and  x9, x23, #0xffff
    add  x0, x8, x9, lsl #23
    bl mmap_commit_unity_slab_chunk

    ldr x10, [sp, #0x2a0]
    ldr x11, [sp, #0x2a8]
    msr fpcr, x10
    msr fpsr, x11
    ldp q30, q31, [sp, #0x280]
    ldp q28, q29, [sp, #0x260]
    ldp q26, q27, [sp, #0x240]
    ldp q24, q25, [sp, #0x220]
    ldp q22, q23, [sp, #0x200]
    ldp q20, q21, [sp, #0x1e0]
    ldp q18, q19, [sp, #0x1c0]
    ldp q16, q17, [sp, #0x1a0]
    ldp q14, q15, [sp, #0x180]
    ldp q12, q13, [sp, #0x160]
    ldp q10, q11, [sp, #0x140]
    ldp q8,  q9,  [sp, #0x120]
    ldp q6,  q7,  [sp, #0x100]
    ldp q4,  q5,  [sp, #0xe0]
    ldp q2,  q3,  [sp, #0xc0]
    ldp q0,  q1,  [sp, #0xa0]
    ldp x18, x30, [sp, #0x90]
    ldp x16, x17, [sp, #0x80]
    ldp x14, x15, [sp, #0x70]
    ldp x12, x13, [sp, #0x60]
    ldp x10, x11, [sp, #0x50]
    ldp x8,  x9,  [sp, #0x40]
    ldp x6,  x7,  [sp, #0x30]
    ldp x4,  x5,  [sp, #0x20]
    ldp x2,  x3,  [sp, #0x10]
    ldp x0,  x1,  [sp, #0x00]
    add sp, sp, #0x2b0

    adrp x8, genshin_unity_slab_aligned_slot
    add  x8, x8, :lo12:genshin_unity_slab_aligned_slot
    ldr  x8, [x8]
    ldr  x8, [x8]
    and  x9, x23, #0xffff
    adds x26, x8, x9, lsl #23
    /* x8 is dead after the replaced selector (the continuation stores x26
     * and branches on NZCV), so it is the only safe indirect-branch scratch
     * at this non-call instruction boundary. */
    adrp x8, genshin_unity_slab_activate_continue
    add  x8, x8, :lo12:genshin_unity_slab_activate_continue
    ldr  x8, [x8]
    br x8
    .size genshin_unity_slab_activate_dispatch, .-genshin_unity_slab_activate_dispatch

    .section .note.GNU-stack,"",%progbits
