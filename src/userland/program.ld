ENTRY(_start)

/* Every loaded program links here, not src/boards/pc-bios/link.ld --
 * see runtime.c's own comment for why this has to be a wholly separate
 * link. Fixed at PROGRAM_VBASE (include/vajra/loader.h) because
 * there's no relocation support here, same simplifying choice
 * link.ld itself already makes for the kernel image: link at the exact
 * address this will run at. */
SECTIONS
{
    . = 0x10000000;

    .text : {
        *(.text.start) /* _start (hello.c) -- guaranteed first, so entry_offset is always 0 */
        *(.text*)
    }

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss : {
        *(.bss*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
        *(.pdata)
        *(.xdata)
    }
}
