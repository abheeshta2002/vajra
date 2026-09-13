bits 16
org 0x7C00

; ============================================================
; VAJRA BOOTLOADER - V0.29 Stabilization Pass
; ============================================================
; V0.29 fixes applied here:
;
;   1. Segments (DS/ES/SS) and SP are explicitly initialized
;      before any code relies on them. Previously nothing set
;      them; every BIOS call (int 0x13, int 0x10) implicitly
;      uses SS:SP, and boot_drive is addressed relative to DS.
;      It happened to work because BIOS/QEMU left them at 0,
;      but that was luck, not a guarantee.
;
;   2. Kernel read switched from INT 13h AH=02 (CHS) to AH=42h
;      (LBA Extended Read), and bumped from 16 to 50 sectors
;      (8KB -> 25KB of headroom for the kernel to grow into).
;
;      This took a lot of empirical debugging to get right, so
;      the reasoning is recorded here for future versions:
;
;      - Plain CHS (AH=02h) reads become unreliable past ~17
;        sectors from sector 2 on this disk's geometry (a
;        standard 1.44MB-floppy-shaped CHS layout: 18
;        sectors/track) -- confirmed by an actual failing boot,
;        not assumed from a spec reading.
;
;      - Switching to LBA (AH=42h) removes the CHS ceiling, but
;        a single Disk Address Packet transfer is still capped
;        at 64KB (one real-mode segment:offset window). Reading
;        too much in one call overflows it (confirmed: AH=0x0E).
;
;      - The real trap: reading a LARGE amount of kernel data
;        straight to 0x1000 makes the read overwrite the boot
;        sector's OWN code at 0x7C00-0x7DFF while it is still
;        executing (this boot sector lives right above the
;        kernel's load address). A chunked-read attempt at 240
;        sectors corrupted its own Disk Address Packet mid-read
;        this way; a later relocate-after-load attempt hit the
;        same wall one step later. Confirmed via direct physical
;        memory + register inspection in QEMU (not guessed).
;
;      - Separately, the kernel's destination range must also
;        stay clear of 0x8000-0xB000, which holds the identity-
;        mapped page tables boot.asm builds and which the CPU
;        keeps actively using (via CR3) through all of the
;        kernel's own early init -- overwriting them corrupts
;        live address translation, not just stale data.
;
;      50 sectors (25KB) landing at 0x1000-0x7400 clears both
;      hazards with comfortable margin (2KB before the boot
;      sector, 22KB before the page tables) and needed no
;      relocation trickery once sized correctly. build.sh
;      enforces this ceiling at build time so a future kernel
;      that outgrows it fails the build loudly instead of
;      silently corrupting itself the way the old code would
;      have. Revisiting this for more headroom (moving the page
;      tables elsewhere, or a properly-isolated relocation
;      stub) is a good candidate for a future version.
; ============================================================

KERNEL_SECTORS equ 50       ; 25600 bytes; build.sh enforces this ceiling

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00      ; stack grows down, away from our own code
    sti

    mov [boot_drive], dl

    ; ========================================
    ; Load Vajra kernel
    ; ========================================

    mov ah, 0x41        ; INT13 Extensions - Installation Check
    mov bx, 0x55AA
    mov dl, [boot_drive]
    int 0x13
    jc disk_error
    cmp bx, 0xAA55
    jne disk_error

    mov si, dap
    mov ah, 0x42        ; Extended Read Sectors
    mov dl, [boot_drive]
    int 0x13
    jc disk_error

    ; ========================================
    ; V0.30: Query the BIOS memory map (E820) and leave it at a
    ; fixed physical address (0x20000) for the kernel to read once
    ; it's running. This has to happen here, in real mode -- E820
    ; is a real-mode BIOS service (INT 15h), unavailable once we've
    ; left real mode below. The kernel's memory manager uses this
    ; to learn how much RAM this machine actually has, instead of
    ; assuming a fixed 16MB like the old allocator did.
    ;
    ; Standard E820 protocol: call repeatedly with EBX carrying a
    ; continuation value (0 to start); each call fills one entry at
    ; ES:DI and returns the next continuation value in EBX (0 means
    ; that was the last entry). ES:DI can't reach 0x20000 directly
    ; (DI alone maxes out at 0xFFFF), so ES is set to 0x2000 here
    ; and restored to 0 afterward for the rest of boot.
    ; ========================================

    mov ax, 0x2000
    mov es, ax
    xor edi, edi
    mov edi, 8              ; leave room for a small header at ES:0
    xor ebx, ebx
    xor ebp, ebp             ; entry count so far

.e820_loop:
    mov eax, 0xE820
    mov ecx, 24
    mov edx, 0x534D4150      ; 'SMAP'
    int 0x15
    jc .e820_done            ; error, or this BIOS doesn't support E820 at all
    cmp eax, 0x534D4150
    jne .e820_done
    cmp ebp, 32
    jae .e820_done           ; cap at 32 entries -- plenty for any normal map
    inc ebp
    add edi, 24
    test ebx, ebx
    jz .e820_done            ; ebx==0 means that was the last entry
    jmp .e820_loop

.e820_done:
    mov dword [es:0], 0x45383230   ; 'E820' magic, so the kernel can tell this data is valid
    mov [es:4], ebp

    xor ax, ax
    mov es, ax

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

; ========================================
; Disk Address Packet for INT 13h AH=42h
; ========================================
align 4
dap:
    db 0x10             ; packet size
    db 0                ; reserved
    dw KERNEL_SECTORS   ; number of sectors to read
    dw 0x1000           ; transfer buffer offset
    dw 0x0000           ; transfer buffer segment
    dq 1                ; starting LBA (sector right after the boot sector)

error_message:
    db "Disk read error!", 0


times 510-($-$$) db 0
dw 0xAA55
