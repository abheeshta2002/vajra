bits 64

global _start
extern kernel_main
extern __bss_start
extern __bss_end

section .text
_start:
    mov rsp, 0x80000    ; same known-good bootstrap stack the asm kernel used

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
