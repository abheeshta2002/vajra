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
    mov rsp, 0x88000

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
