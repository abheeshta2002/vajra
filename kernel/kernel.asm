bits 32
org 0x1000

start:
    mov esi, message
    mov edi, 0xB8000
    mov ah, 0x07

print:
    lodsb
    cmp al, 0
    je hang

    stosw
    jmp print

hang:
    cli
    hlt
    jmp hang

message:
    db "Vajra 32-bit protected mode!", 0