ENTRY(_start)

/*
 * The link address is nominal: everything is PC-relative (start.S, and C built without
 * position-dependent data). 0x80080000 is where an Exynos S-Boot usually puts the kernel
 * (DRAM base 0x80000000 + text_offset); a21sdiag reports where it really ran.
 */
SECTIONS
{
    . = 0x80080000;

    .text : {
        *(.text.boot)
        *(.text .text.*)
    }

    .rodata : {
        *(.rodata .rodata.*)
    }

    .data : {
        *(.data .data.*)
    }

    .bss : {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(16);
        __stack_bottom = .;
        . += 0x4000;
        __stack_top = .;
        . = ALIGN(16);
        __fault_stack_bottom = .;
        . += 0x1000;
        __fault_stack_top = .;
        __bss_end = .;
    }

    . = ALIGN(16);
    __loader_end = .;

    /DISCARD/ : { *(.rela*) *(.got*) *(.dynamic) *(.dynsym) *(.dynstr) *(.interp) *(.note*) *(.comment) }
}

/* Android's Image starts at CHAIN_OFFSET (tools/chainboot.py): our .bss and stacks must end before it. */
ASSERT(__loader_end - _start <= 0x40000, "a21sdiag: program + bss + stacks exceed CHAIN_OFFSET")
