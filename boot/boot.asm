bits 16
org 0x7C00

start:
    mov [boot_drive], dl

    ; ========================================
    ; Load Vajra kernel
    ; ========================================

    mov ah, 0x02

    ; Load 16 sectors = 8192 bytes
    mov al, 16

    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [boot_drive]

    ; Kernel destination
    mov bx, 0x1000

    int 0x13
    jc disk_error

    cli

    ; ========================================
    ; Load GDT
    ; ========================================

    lgdt [gdt_descriptor]

    ; ========================================
    ; Protected Mode
    ; ========================================

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    jmp 0x08:protected_mode


bits 32

protected_mode:

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; ========================================
    ; Page tables
    ;
    ; Keep these away from kernel memory.
    ; ========================================

    mov edi, 0x8000
    xor eax, eax
    mov ecx, 3072
    rep stosd

    ; PML4 → PDPT
    mov dword [0x8000], 0x9003

    ; PDPT → Page Directory
    mov dword [0x9000], 0xA003

    ; Identity map first 2 MB
    mov dword [0xA000], 0x0083

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4
    mov eax, 0x8000
    mov cr3, eax

    ; ========================================
    ; Enable Long Mode
    ; ========================================

    mov ecx, 0xC0000080
    rdmsr

    or eax, 1 << 8
    wrmsr

    ; Enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; ========================================
    ; Enter 64-bit mode
    ; ========================================

    jmp 0x18:long_mode


bits 64

long_mode:

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    jmp 0x1000


bits 16

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


; ========================================
; GDT
; ========================================

align 8

gdt_start:

    ; Null
    dq 0x0000000000000000

    ; 32-bit code
    dq 0x00CF9A000000FFFF

    ; Data
    dq 0x00CF92000000FFFF

    ; 64-bit code
    dq 0x00AF9A000000FFFF

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