/*
 * Entry for a21sdiag, booted by Samsung's S-Boot as a Linux arm64 Image (MMU and caches off).
 *
 * Position independent: where S-Boot puts the kernel on the Exynos 850 is one of the things this
 * program finds out, so nothing here (or in the C) may depend on the link address. Only adr/adrp
 * are used; the stack is in our own .bss, inside the first CHAIN_OFFSET bytes (link.ld checks).
 */
.global _start
.section .text.boot

_start:
    b       entry               /* code0 */
    .word   0                   /* code1 */
    .quad   0x80000             /* text_offset: the same as Android's Image (chainboot.py checks) */
    .quad   0x02000000          /* image_size: rewritten by chainboot.py */
    .quad   0x0a                /* flags: LE, 4K pages, anywhere in memory */
    .quad   0                   /* res2: the Image's length (chainboot.py) */
    .quad   0                   /* res3: the payload's offset (chainboot.py) */
    .quad   0
    .ascii  "ARM\x64"
    .word   0

entry:
    msr     daifset, #0xf
    mov     x19, x0             /* the dtb S-Boot hands over */
    mov     x20, x1
    mov     x21, x2
    mov     x22, x3

    mrs     x1, CurrentEL
    lsr     x1, x1, #2
    adr     x2, diag_vectors
    cmp     x1, #2
    b.ne    1f
    msr     vbar_el2, x2
    b       2f
1:  msr     vbar_el1, x2
2:  isb

    adrp    x0, __stack_top
    add     x0, x0, :lo12:__stack_top
    mov     sp, x0

    adrp    x0, __bss_start
    add     x0, x0, :lo12:__bss_start
    adrp    x1, __stack_bottom          /* clear .bss up to the stack (the stack needs no clearing) */
    add     x1, x1, :lo12:__stack_bottom
3:  cmp     x0, x1
    b.hs    4f
    str     xzr, [x0], #8
    b       3b
4:
    mov     x0, x19
    mov     x1, x20
    mov     x2, x21
    mov     x3, x22
    bl      diag_main
5:  wfe
    b       5b

/*
 * chain_stub(dtb, dst, src, len, entry): copy the Linux Image down over this program and enter
 * it as S-Boot would have (x0 = dtb, x1-x3 = 0). C copies this stub out of the way first, since
 * the copy overwrites the code that does it. Position independent; len is a multiple of 8 and
 * dst < src, so a forward copy is safe.
 *
 * S-Boot loaded the boot image with its caches on and cleaned them to the PoC, but need not
 * have invalidated them: clean lines holding what S-Boot wrote at these addresses (our program
 * and the Image, 0x40000 further up) can still be in the caches. Our copy runs with the MMU off
 * and goes straight to DRAM, so the moment Linux turns its caches on it would read those stale
 * lines instead of its own code (the first a21sdiag hung right there). Invalidate the
 * destination by VA (== PA with the MMU off) after the copy: invalidate, not clean, so a stale
 * line can never be written back over the copy.
 */
.global chain_stub
.global chain_stub_end
chain_stub:
    mov     x7, x1              /* dst */
    mov     x8, x3              /* len */
1:  ldr     x6, [x2], #8
    str     x6, [x1], #8
    subs    x3, x3, #8
    b.hi    1b
    dsb     sy
    mrs     x9, ctr_el0
    ubfx    x9, x9, #16, #4     /* DminLine: log2 of the line in words */
    mov     x10, #4
    lsl     x10, x10, x9        /* smallest D-cache line, bytes */
    sub     x11, x10, #1
    bic     x6, x7, x11         /* from the line holding dst */
    add     x8, x7, x8          /* to dst + len */
2:  dc      ivac, x6
    add     x6, x6, x10
    cmp     x6, x8
    b.lo    2b
    dsb     sy
    ic      iallu
    dsb     sy
    isb
    mov     x1, xzr
    mov     x2, xzr
    mov     x3, xzr
    br      x4
chain_stub_end:

/* Any exception (SError included): hand ESR/ELR/FAR and the vector offset to C, which reports and moves on. */
.macro vec off
    .balign 0x80
    mov     x0, #\off
    b       fault_common
.endm

fault_common:
    mrs     x4, CurrentEL
    lsr     x4, x4, #2
    cmp     x4, #2
    b.ne    1f
    mrs     x1, esr_el2
    mrs     x2, elr_el2
    mrs     x3, far_el2
    b       2f
1:  mrs     x1, esr_el1
    mrs     x2, elr_el1
    mrs     x3, far_el1
2:  adrp    x5, __fault_stack_top
    add     x5, x5, :lo12:__fault_stack_top
    mov     sp, x5
    bl      diag_fault
3:  wfe
    b       3b

    .balign 0x800
diag_vectors:
    vec 0x000
    vec 0x080
    vec 0x100
    vec 0x180
    vec 0x200
    vec 0x280
    vec 0x300
    vec 0x380
    vec 0x400
    vec 0x480
    vec 0x500
    vec 0x580
    vec 0x600
    vec 0x680
    vec 0x700
    vec 0x780
