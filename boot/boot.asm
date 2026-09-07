bits 16
org 0x7C00

start:
    mov [boot_drive], dl

    ; Load kernel from disk
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]
    mov bx, 0x1000
    int 0x13

    jc disk_error

    ; Disable interrupts
    cli

    ; Load Global Descriptor Table
    lgdt [gdt_descriptor]

    ; Enable Protected Mode
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax

    ; Far jump into 32-bit code segment
    jmp 0x08:protected_mode

disk_error:
    mov si, error_message

print_error:
    lodsb
    cmp al, 0
    je hang

    mov ah, 0x0E
    int 0x10
    jmp print_error

hang:
    cli
    hlt
    jmp hang


; ------------------------------------------------
; 32-bit Protected Mode entry
; ------------------------------------------------

bits 32

protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    jmp 0x1000


; ------------------------------------------------
; Global Descriptor Table
; ------------------------------------------------

gdt_start:

gdt_null:
    dq 0

gdt_code:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10011010b
    db 11001111b
    db 0x00

gdt_data:
    dw 0xFFFF
    dw 0x0000
    db 0x00
    db 10010010b
    db 11001111b
    db 0x00

gdt_end:


gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start


boot_drive:
    db 0

error_message:
    db "Disk read error!", 0


times 510-($-$$) db 0
dw 0xAA55