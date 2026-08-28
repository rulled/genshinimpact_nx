/* Caller-local bridge for the four Mmoron managed-path instructions replaced
 * at guest RVA 0x96C4324.
 *
 * x20 is the live MmoronInitParams object.  Pass it through the normal C ABI,
 * then resume with x0 holding Path.Combine's result.  The C helper preserves
 * every AArch64 callee-saved register, including x20; the surrounding managed
 * method restores its saved LR in its own epilogue. */

    .section .text.genshin_mmoron_directory_sequence_dispatch,"ax",%progbits
    .p2align 2
    .global genshin_mmoron_directory_sequence_dispatch
    .hidden genshin_mmoron_directory_sequence_dispatch
    .type genshin_mmoron_directory_sequence_dispatch, %function
genshin_mmoron_directory_sequence_dispatch:
    mov x0, x20
    bl genshin_mmoron_directory_sequence_bridge
    adrp x16, genshin_mmoron_directory_continue
    add x16, x16, :lo12:genshin_mmoron_directory_continue
    ldr x16, [x16]
    br x16
    .size genshin_mmoron_directory_sequence_dispatch, .-genshin_mmoron_directory_sequence_dispatch

    .section .note.GNU-stack,"",%progbits
