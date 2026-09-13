bits 64

global _start
extern kernel_main

section .text
_start:
    mov rsp, 0x80000    ; same known-good bootstrap stack the asm kernel used
    call kernel_main
.hang:
    hlt
    jmp .hang
