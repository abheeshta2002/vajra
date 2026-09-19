bits 16
org 0x0000
default abs

; ============================================================
; VAJRA SMP: Application Processor trampoline.
;
; Assembled as its OWN standalone flat binary (see tools/build-c.ps1),
; exactly like src/boards/pc-bios/boot.asm is -- not linked into the
; main kernel image, because it has to run from a fixed low physical
; address in 16-bit real mode before anything resembling the kernel's
; own addressing (paging, long mode, its own GDT) exists on this core
; at all.
;
; hal/x86_64/smp.c copies these bytes verbatim to physical 0x0E000
; (originally 0x70000, then 0x96000 -- see smp.c's own comment for the
; full relocation history, most recently boot.asm's "structural fix":
; everything fixed and low-memory now lives BELOW the kernel's own
; 0x20000 load address instead of competing with its .bss growth
; above it) before sending the wake-up IPI sequence (INIT then SIPI,
; vector 0x0E -- SIPI's vector field IS the destination page number,
; so vector 0x0E means "start executing at physical 0x0E000"). An
; Application Processor woken this way begins execution with
; CS=0x0E00, IP=0, which is exactly this file's own org 0x0000: every
; label below is already the correct real-mode offset within that
; segment, no runtime relocation needed for the 16-bit portion.
;
; Once in 32-bit protected mode, segment bases are 0 (flat), so a
; label's plain assembled value (a small offset near 0, thanks to
; org 0x0000) would point near physical address 0 -- wrong, since this
; code actually lives at 0x0E000+. TRAMPOLINE_BASE below corrects for
; that explicitly wherever a flat/linear address is needed (the two far
; jumps, and the GDT descriptor's base field) -- everything that stays
; in real-mode segment:offset form (e.g. `lgdt [gdt_descriptor]`, DS
; already pointed at segment 0x0E00) does not need it.
;
; Deliberately reuses the BSP's own already-built page tables
; (physical 0x08000 -- see boot.asm's own comment) instead of building
; a redundant second identity map: those tables are never written
; again after boot.asm finishes with them, so both cores can safely
; read them concurrently, and this AP never needs any mapping the BSP
; doesn't already have (see hal/x86_64/paging.c's hal_map_lapic_mmio()
; for the one addition made to those same tables before this trampoline
; is ever copied anywhere).
; ============================================================

%define TRAMPOLINE_BASE 0x0E000

start:
    cli
    mov ax, 0x0E00
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0xFFF0

    lgdt [gdt_descriptor]

    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; `dword` forces a 32-bit-offset far jump -- TRAMPOLINE_BASE alone
    ; (0x70000) already exceeds a 16-bit offset's range, so the default
    ; 16-bit far jump this assembles to without the override cannot
    ; even encode the target address.
    jmp dword 0x08:TRAMPOLINE_BASE + protected_mode

bits 32
protected_mode:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; PAE, then the BSP's existing PML4 (0x08000 -- boot.asm), then
    ; long mode -- identical sequence to boot.asm's own long-mode
    ; transition (see its comment), minus building the page tables
    ; themselves: they already exist and are never modified again.
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, 0x08000
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    jmp dword 0x18:TRAMPOLINE_BASE + long_mode_ap

bits 64
long_mode_ap:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax

    ; A fixed, dedicated stack for this AP -- see hal/x86_64/smp.c's
    ; own comment for the full low-memory map this address was chosen
    ; against. Never shared with the BSP's own stack or any actor's.
    mov rsp, 0x11000

    ; Hand off into the real, linked kernel image: this trampoline is a
    ; separately assembled, position-independent blob with zero
    ; visibility into the main kernel's symbol table, so it cannot
    ; simply `call` a named C function. hal/x86_64/smp.c instead
    ; leaves that function's address in a fixed mailbox cell
    ; (0x0EFF8, just past this code, within the same page) before
    ; ever triggering the SIPI that starts this file running.
    mov rax, [0x0EFF8]
    call rax

.halt:
    hlt
    jmp .halt

align 8
gdt_start:
    dq 0x0000000000000000       ; null
    dq 0x00CF9A000000FFFF       ; 0x08: 32-bit code
    dq 0x00CF92000000FFFF       ; 0x10: data
    dq 0x00AF9A000000FFFF       ; 0x18: 64-bit code
gdt_end:
gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd TRAMPOLINE_BASE + gdt_start
