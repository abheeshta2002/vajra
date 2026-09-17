ENTRY(_start)

SECTIONS
{
    /* Milestone 13: moved from 0x1000 to 0x20000 -- the kernel image
       outgrew the room available below 0x7C00 (the boot sector's own
       load address, a hard ceiling on how far anything loaded at
       0x1000 can grow -- see src/boards/pc-bios/boot.asm's own "fix
       #3" comment for the full reasoning and why this address in
       particular). Must stay in sync with boot.asm's DAP transfer
       target (segment 0x2000:offset 0x0000) and its `jmp 0x20000`
       into long mode. */
    . = 0x20000;

    .text : {
        *(.text*)
    }

    /* Code that must be executable from ring 3 (actor entry points,
       and the tiny syscall-invoking wrappers they call) lives here
       instead of the ordinary .text above. Page-aligned on both ends
       so hal/x86_64/paging.c can mark exactly these whole pages
       user-accessible in every actor's address space while leaving
       the rest of the kernel's code supervisor-only -- see its own
       comment for why string/data constants referenced from here
       don't ALSO need to move: taking an address is unrestricted at
       any privilege level, only dereferencing it is checked, and the
       only code that ever dereferences a syscall argument is the
       kernel's own syscall handler, running at CPL 0. */
    . = ALIGN(4096);
    __user_text_start = .;
    .user_text : {
        *(.user_text)
    }
    . = ALIGN(4096);
    __user_text_end = .;

    .rodata : {
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    /* Marked so start.asm can zero this range explicitly at boot --
       .bss isn't part of the loaded flat binary at all (SHT_NOBITS),
       so nothing zeroes this memory automatically the way a real
       loader normally would. C code assumes uninitialized static/
       global variables start at zero; without this, they'd contain
       whatever leftover memory happened to be there. */
    __bss_start = .;
    .bss : {
        *(.bss*)
        *(COMMON)
    }
    __bss_end = .;

    /DISCARD/ : {
        *(.eh_frame)
        *(.comment)
        *(.note*)
        *(.pdata)
        *(.xdata)
    }
}
