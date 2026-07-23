.section .text.drastic_jit_codegen_original, "ax", %progbits
.align 2

.global drastic_jit_codegen_original
.type drastic_jit_codegen_original, %function
drastic_jit_codegen_original:
    // Exact prologue replaced at libdrastic_arm64.so + 0x9558c.
    stp x28, x27, [sp, #-96]!
    stp x26, x25, [sp, #16]
    stp x24, x23, [sp, #32]
    stp x22, x21, [sp, #48]

    adrp x16, drastic_jit_codegen_resume
    add  x16, x16, :lo12:drastic_jit_codegen_resume
    ldr  x16, [x16]
    br   x16
.size drastic_jit_codegen_original, . - drastic_jit_codegen_original

.section .text.drastic_jit_resolve_original, "ax", %progbits
.align 2

.global drastic_jit_resolve_original
.type drastic_jit_resolve_original, %function
drastic_jit_resolve_original:
    // Exact prologue replaced at libdrastic_arm64.so + 0x95e08.
    str x21, [sp, #-48]!
    stp x20, x19, [sp, #16]
    stp x29, x30, [sp, #32]
    add x29, sp, #32

    adrp x16, drastic_jit_resolve_resume
    add  x16, x16, :lo12:drastic_jit_resolve_resume
    ldr  x16, [x16]
    br   x16
.size drastic_jit_resolve_original, . - drastic_jit_resolve_original

// Mid-function collision-chain stores performed after code generation. The
// source header is still dirty in the RW view, and the destination is an RX
// cache address, so both accesses are redirected before the site is added to
// the current thread's deferred publication batch.
.macro DRASTIC_LINK_STORE name, base_reg, resume_symbol
.section .text.\name, "ax", %progbits
.align 2
.global \name
.type \name, %function
\name:
    adrp x16, drastic_jit_alias_delta
    add  x16, x16, :lo12:drastic_jit_alias_delta
    ldr  x16, [x16]
    add  x17, x0, x16
    ldur w8, [x17, #-4]
    add  x17, \base_reg, x9
    add  x17, x17, x16
    str  w8, [x17]

    // __aarch64_read_tp inline: ThreadVars::tls_tp is at +504.
    mrs  x16, tpidrro_el0
    ldr  x16, [x16, #504]
    add  x16, x16, #:tprel_hi12:g_pending_publish, lsl #12
    add  x16, x16, #:tprel_lo12_nc:g_pending_publish
    ldr  w15, [x16, #108]
    cmp  w15, #64
    b.hs 1f
    add  x14, x16, #112
    str  x17, [x14, x15, lsl #3]
    add  w15, w15, #1
    str  w15, [x16, #108]
    b    2f
1:
    mov  w15, #1
    str  w15, [x16, #624]
2:
    adrp x16, \resume_symbol
    add  x16, x16, :lo12:\resume_symbol
    ldr  x16, [x16]
    br   x16
.size \name, . - \name
.endm

DRASTIC_LINK_STORE drastic_jit_primary_link_store, x23, drastic_jit_primary_link_resume
DRASTIC_LINK_STORE drastic_jit_nested_link_store, x22, drastic_jit_nested_link_resume
