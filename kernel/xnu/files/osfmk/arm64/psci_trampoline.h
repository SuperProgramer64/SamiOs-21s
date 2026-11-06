/* SPDX-License-Identifier: APSL-2.0
 * Copyright (c) 2026 The HakiOS authors. All rights reserved.
 * Low-memory entry witness. All instructions are position independent, use
 * only x16/x17, and preserve the PSCI context in x0. No stack or MMU needed.
 * The loader reserves a page; code/literal occupy 56 bytes, record 128..191.
 */
#ifndef HAKIOS_PSCI_TRAMPOLINE_H
#define HAKIOS_PSCI_TRAMPOLINE_H
static void hakios_psci_trampoline_build(volatile uint32_t *p, uint64_t entry)
{
    static const uint32_t insns[] = {
        0x10000410, /* adr x16, entry+128 */
        0xd53800b1, /* mrs x17, MPIDR_EL1 */
        0xf9000611, /* str x17, [x16, #8] */
        0xd5384251, /* mrs x17, CurrentEL */
        0xf9000a11, /* str x17, [x16, #16] */
        0x10ffff71, /* adr x17, entry */
        0xf9000e11, /* str x17, [x16, #24] */
        0xd2800031, /* mov x17, #1 */
        0xf9000211, /* str x17, [x16] */
        0xd5033f9f, /* dsb sy: publish the record before the high jump */
        0x58000050, /* ldr x16, literal at offset 48 */
        0xd61f0200, /* br x16 */
    };
    for(unsigned i=0;i<sizeof(insns)/sizeof(insns[0]);i++) p[i]=insns[i];
    p[12]=(uint32_t)entry; p[13]=(uint32_t)(entry>>32);
}
/* Gated Exynos entry, adapted from the mailbox handshake in simpleXNU/ios-a135f
 * by Schoosh and Bruhboy666dev. Independently assembled for this loader's record.
 * One private 256-byte slot per CPU. record[4]: 0 wait, 1 run, 2 CPU_OFF.
 */
static void hakios_psci_trampoline_build_gated(volatile uint32_t *p, uint64_t entry)
{
    static const uint32_t code[] = { 0x10000410, 0xd53800b1, 0xf9000611, 0xd5384251, 0xf9000a11, 0x10ffff71, 0xf9000e11, 0xd2800031, 0xd5033f9f, 0xf9000211, 0xd5033f9f, 0xf9401211, 0xb4fffff1, 0xf100063f, 0x54000081, 0xd5033f9f, 0x58000110, 0xd61f0200, 0xd2800040, 0xf2b08000, 0xd4000003, 0xd503207f, 0x17ffffff, 0xd503201f };
    for(unsigned i=0;i<24;i++) p[i]=code[i];
    p[24]=(uint32_t)entry; p[25]=(uint32_t)(entry>>32);
}
#endif
