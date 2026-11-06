/* SPDX-License-Identifier: MIT
 * Stackless, MMU-off secondary probe. Low entry uses its own PC-relative mailbox,
 * so it also diagnoses firmware losing the PSCI context in x0. The high fragment
 * is copied verbatim to reserved staging RAM and uses no external relocations.
 * Mailbox offsets are checked against the C structure in smp_probe.h.
 */
.section .text.hakios_smp,"ax"
.balign 16
.global hakios_smp_low
hakios_smp_low:
    msr daifset, #0xf
    adrp x19, hakios_smp_mailbox
    add x19, x19, :lo12:hakios_smp_mailbox
    str x0, [x19, #40]
    adr x1, hakios_smp_low
    str x1, [x19, #32]
    mrs x1, mpidr_el1
    str x1, [x19, #8]
    mrs x1, CurrentEL
    str x1, [x19, #16]
    cmp x1, #8
    b.eq 1f
    mrs x1, sctlr_el1
    b 2f
1:  mrs x1, sctlr_el2
2:  str x1, [x19, #24]
    mov x1, #1
    str x1, [x19]
    dsb sy
    ldr x16, [x19, #48]
    cbz x16, 3f
    mov x0, x19
    br x16
3:  mov x0, #2
    movk x0, #0x8400, lsl #16
    smc #0
    mov x1, #0xff
    str x1, [x19]
    dsb sy
4:  wfe
    b 4b

.balign 16
.global hakios_smp_high, hakios_smp_high_end
hakios_smp_high:
    msr daifset, #0xf
    mov x19, x0
    /* A direct CPU_ON needs to populate the registers the low path records. */
    ldr x1, [x19]
    cbnz x1, 2f
    str x0, [x19, #40]
    mrs x1, mpidr_el1
    str x1, [x19, #8]
    mrs x1, CurrentEL
    str x1, [x19, #16]
    cmp x1, #8
    b.eq 1f
    mrs x1, sctlr_el1
    b 3f
1:  mrs x1, sctlr_el2
3:  str x1, [x19, #24]
2:  adr x1, hakios_smp_high
    str x1, [x19, #56]
    mov x1, #2
    str x1, [x19]
    dsb sy
    mov x0, #2
    movk x0, #0x8400, lsl #16
    smc #0
    mov x1, #0xff
    str x1, [x19]
    dsb sy
4:  wfe
    b 4b
hakios_smp_high_end:
