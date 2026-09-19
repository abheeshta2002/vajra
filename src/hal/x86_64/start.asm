bits 64

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text
_start:
    ; Milestone 13: moved from 0x80000 -- the kernel's own .bss now
    ; extends to ~0x80a0c (adding a PCI scanner + virtio-net driver, on
    ; top of the Milestone-13 kernel-load relocation from 0x1000 to
    ; 0x20000, pushed it there for the first time), which silently
    ; overlapped this stack's top. Confirmed by an actual hang, not
    ; guessed: the .bss-zeroing loop below was zeroing live return
    ; addresses out from under itself. See hal/x86_64/smp.c's own
    ; comment for the AP trampoline's matching relocation -- the same
    ; growth swallowed both.
    ;
    ; Milestone 18: moved again, from 0x88000 -- the SAME bug,
    ; recurring a third time. By this milestone .bss reached 0x88c0c
    ; (Phase 17's on-disk directory plus Phase 18's keyboard/RTC
    ; drivers, shell, and raised capability-table quota all added real
    ; code/string size, which shifts where .bss STARTS just as much as
    ; any single array's own growth does), silently overlapping this
    ; stack's top again -- confirmed by an actual hang (boot stalled
    ; right after "IDT installed.", zero fault entries in the QEMU
    ; debug log, exactly the earlier two instances of this bug class'
    ; own signature), found via the same ELF-relink + llvm-nm technique
    ; those used, not guessed.
    ;
    ; Shrinking the newly-added structures back down would only have
    ; reclaimed a few hundred bytes against a multi-KB overrun -- most
    ; of the growth is genuine new code/string size, not a single
    ; oversized array this time. Relocating INTO the previously unused
    ; 32KB gap between the old stack top (0x88000) and the page tables
    ; (0x90000, boot.asm) instead gives real headroom for ordinary
    ; future growth, rather than repeating a same-size fix that just
    ; failed to scale past one more milestone.
    ;
    ; Nudged again, same milestone: the console windowing-compositor
    ; rewrite (right after this fix landed) pushed .bss to 0x8ec0c,
    ; leaving only ~1KB before 0x8F000 -- not enough real margin for
    ; the stack's OWN usage (which grows downward from this address) to
    ; stay clear of .bss without risking exactly the corruption this
    ; whole relocation exists to prevent. Moved to 0x8FF00, only 256
    ; bytes below the page tables at 0x90000 (comfortably enough --
    ; boot-time stack usage before the scheduler exists is shallow,
    ; nowhere near that), to reclaim the rest of the gap for .bss.
    ;
    ; Structural fix, same milestone: the desktop's own further growth
    ; pushed .bss to 0x900fc -- PAST the page tables' own first bytes
    ; this time, a genuine collision, not just a shrinking margin. This
    ; was the sixth time this exact bug class hit, and every fix so far
    ; nudged the stack (and the page tables/E820 map/AP trampoline it
    ; sits next to) along the SAME ~458KB budget between the kernel's
    ; 0x20000 load address and 0x90000 -- a budget that's now provably
    ; too small for this kernel's real growth rate. See boot.asm's own
    ; "structural fix" comment: the page tables, E820 map, and AP
    ; trampoline/stack all moved BELOW 0x20000 instead, into space the
    ; kernel itself used to occupy before Milestone 13 moved it up --
    ; genuinely free ever since, and permanently clear of kernel .bss
    ; growth (which only ever grows UPWARD from 0x20000). The boot
    ; stack moves there too, to 0x1F000 -- just below the kernel's own
    ; load address, with ~56KB of headroom below it before the AP's own
    ; stack (0x11000, smp.c) even starts, rather than the ~250-3500
    ; byte margins every previous fix here was reduced to living with.
    mov rsp, 0x1F000

    ; Zero .bss before calling into C. This memory isn't part of the
    ; loaded flat binary at all (see link.ld's note), so without this
    ; explicit step, C's "uninitialized statics/globals start at
    ; zero" guarantee would silently not hold.
    mov rdi, __bss_start
    mov rcx, __bss_end
    sub rcx, rdi
    xor al, al
    rep stosb

    call kernel_main
.hang:
    hlt
    jmp .hang
