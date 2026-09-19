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
;        stay clear of wherever the identity-mapped page tables
;        boot.asm builds live, since the CPU keeps actively using
;        them (via CR3) through all of the kernel's own early
;        init -- overwriting them corrupts live address
;        translation, not just stale data. These originally lived
;        at 0x8000-0xB000, right after the kernel's own load
;        address, on the assumption the kernel image would always
;        stay small. That assumption broke once the kernel's .bss
;        (zeroed at runtime by start.asm -- unlike the on-disk
;        image below, its size isn't bounded by KERNEL_SECTORS at
;        all, so it's easy to grow without noticing) reached
;        0x8000 and the .bss-zeroing loop overwrote the live page
;        tables out from under CR3. They now live at 0x90000+,
;        above the 0x80000 boot stack instead of right after the
;        kernel image, so kernel/.bss growth and page-table
;        placement can no longer collide short of the kernel
;        image + .bss together approaching half a megabyte.
;
;      50 sectors (25KB) landing at 0x1000-0x7400 cleared the
;      boot-sector hazard with comfortable margin (2KB to spare)
;      and needed no relocation trickery once sized correctly.
;
;   3. Milestone 13 (roadmap Phase 12): the kernel image itself
;      outgrew that whole budget -- adding a PCI scanner and a
;      virtio-net driver pushed it past 50 sectors (28924 bytes,
;      57 sectors) for the first time, and 0x7C00 (this boot
;      sector's own load address) is a HARD ceiling on how far
;      anything loaded at 0x1000 can grow before the read starts
;      overwriting this code while it's still running -- the exact
;      failure mode fix #2 above already had to debug once. Rather
;      than trim the margin to fit (a wall that would just get hit
;      again next time the kernel grows), the kernel's load address
;      moved from 0x1000 to 0x20000 (128KB) -- comfortably clear of
;      this boot sector at 0x7C00 in one direction, and clear of the
;      page tables (0x90000+) and E820 map (0x94000+) in the other.
;      KERNEL_SECTORS raised to 120 (61440 bytes) accordingly --
;      still comfortably under a single Disk Address Packet transfer's
;      64KB cap (fix #2's own limit), so no chunked-read complexity is
;      needed yet.
;
;      This moved the ON-DISK-IMAGE hazard, but not the whole
;      problem: the kernel's .bss (zeroed at runtime, so invisible to
;      KERNEL_SECTORS and easy to grow without noticing -- the exact
;      trap fix #2's page-table relocation already names) turned out
;      to extend to ~0x80a0c, well past where the boot stack
;      (hal/x86_64/start.asm, then 0x80000) and the AP trampoline
;      (hal/x86_64/smp.c, then 0x70000) still lived -- both silently
;      swallowed by live kernel .bss. Confirmed by an actual hang
;      (the AP trampoline's own zeroing loop was overwriting return
;      addresses on what it didn't know was now also the live boot
;      stack), not caught by inspection first. Both relocated again,
;      to 0x88000 and 0x96000 respectively -- see their own files'
;      comments. This is the same recurring bug class as fix #2, one
;      structure later: growing kernel/.bss vs. a fixed low-memory
;      address nothing is protecting from being outgrown. There is
;      still no general mechanism preventing a FOURTH collision next
;      time something grows -- worth remembering before trusting any
;      change near these fixed addresses on inspection alone.
;
;      Fix #4 (Milestone 18): the predicted fourth collision, right on
;      schedule -- .bss reached 0x88c0c (Phase 17's on-disk directory
;      plus Phase 18's keyboard/RTC/shell all adding real code/string
;      size, not one runaway array this time), overlapping the boot
;      stack's top again. Moved once more, to 0x8F000 -- see
;      hal/x86_64/start.asm's own comment for why this one aims for
;      real headroom (the previously unused 32KB gap below the page
;      tables) instead of repeating a same-size fix.
;
;      Fix #5 (still Milestone 18, same session): the desktop
;      compositor rewrite (hal/x86_64/console.c) pushed .bss to
;      0x900fc -- PAST 0x90000 itself this time, genuinely colliding
;      with the live page tables' own first bytes, not just their
;      neighborhood. Every fix above was a same-size nudge along the
;      same ~458KB budget between the kernel's 0x20000 load address
;      and the page tables at 0x90000 -- and that whole budget is now
;      provably not enough headroom for this kernel's actual growth
;      rate (six collisions of this same bug class in one project).
;
;      Structural fix instead of a sixth nudge: the page tables, E820
;      map, and AP trampoline/stack all move BELOW the kernel's own
;      0x20000 load address, into the ~128KB the kernel used to
;      occupy before Milestone 13 moved it up to make room for itself
;      -- genuinely UNUSED since that move, not fought over by
;      anything. Kernel .bss only ever grows UPWARD from 0x20000;
;      these structures now live permanently BELOW it, so the two can
;      never collide again regardless of how large the kernel gets
;      (short of it someday exceeding ~640KB total, a different
;      problem for a different day). New layout:
;        0x08000-0x0AFFF  page tables (was 0x90000-0x92FFF)
;        0x0C000           E820 map (was 0x94000)
;        0x0E000-0x0EFFF  AP trampoline (was 0x96000-0x96FFF)
;        0x11000           top of the AP's own stack (was 0x99000)
;        0x1F000           top of the BSP's own boot stack (was
;                           0x8FF00 -- see hal/x86_64/start.asm)
;      All comfortably clear of the boot sector itself (0x7C00-
;      0x7DFF, still live while THIS code runs) and the IVT/BIOS data
;      area (0x0000-0x04FF).
; ============================================================

KERNEL_SECTORS equ 120      ; 61440 bytes; loads at 0x20000 -- see fix #3 above

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
    ; fixed physical address for the kernel to read once it's running.
    ; This has to happen here, in real mode -- E820 is a real-mode
    ; BIOS service (INT 15h), unavailable once we've left real mode
    ; below. The kernel's memory manager uses this to learn how much
    ; RAM this machine actually has, instead of assuming a fixed 16MB
    ; like the old allocator did.
    ;
    ; This originally lived at 0x20000 (ES=0x2000), which worked as
    ; long as the kernel's .bss stayed well below it. .bss is zeroed
    ; at runtime by start.asm and isn't part of the loaded flat binary
    ; at all, so it costs nothing on disk and is easy to grow without
    ; noticing -- exactly what happened when the actor table grew
    ; (more actors -> bigger per-actor static tables, see
    ; hal/x86_64/paging.c and gdt.c) and pushed .bss's end past
    ; 0x20000. start.asm's .bss-zeroing loop then wiped this map
    ; before memory_init() ever got to read it, silently falling back
    ; to a conservative 16MB. This is the exact same failure mode that
    ; hit the page tables once already (see the "Page tables" comment
    ; below) -- moved to 0x94000, past those too, for the same reason:
    ; clear of the kernel image and .bss's growth from below, and clear
    ; of the page tables now living at 0x90000-0x93000.
    ;
    ; Standard E820 protocol: call repeatedly with EBX carrying a
    ; continuation value (0 to start); each call fills one entry at
    ; ES:DI and returns the next continuation value in EBX (0 means
    ; that was the last entry). ES:DI can't reach 0x94000 directly
    ; (DI alone maxes out at 0xFFFF), so ES is set to 0x9400 here
    ; and restored to 0 afterward for the rest of boot.
    ; ========================================

    mov ax, 0x0C00      ; segment 0x0C00 -> base 0x0C000 -- see this file's own "structural fix" comment
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
    ; Placed at 0x08000+ -- BELOW the kernel's own 0x20000 load
    ; address, in the space the kernel itself used to occupy before
    ; Milestone 13 moved it up (see this file's own "structural fix"
    ; comment above for the full reasoning: kernel .bss only grows
    ; UPWARD from 0x20000, so living below it means growth and
    ; page-table placement can never collide again, unlike every
    ; previous placement this project has tried above 0x20000).
    ; ========================================

    mov edi, 0x08000
    xor eax, eax
    mov ecx, 3072
    rep stosd

    ; PML4 → PDPT
    mov dword [0x08000], 0x09003

    ; PDPT → Page Directory
    mov dword [0x09000], 0x0A003

    ; Identity map the first 256MB using 2MB pages (128 entries fit in
    ; one page directory's 512 slots with room to spare). 256MB must
    ; match core/memory.c's MEM_CAP -- the physical allocator there
    ; hands out any page the E820 map reports as usable up to that
    ; cap, so every page it can ever return has to already be mapped
    ; here. Before this, only the first 2MB was mapped: alloc_page()
    ; would happily hand out a page far beyond that (typical QEMU RAM
    ; is 100+MB), and the first write to it -- alloc_page() zeroes
    ; every page it returns -- page-faulted immediately.
    mov edi, 0x0A000
    mov eax, 0x83           ; present + writable + PS (2MB page), base 0
    mov ecx, 128             ; 128 * 2MB = 256MB
.map_pd_loop:
    mov [edi], eax
    add eax, 0x200000
    add edi, 8
    loop .map_pd_loop

    ; Enable PAE
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; Load PML4
    mov eax, 0x08000
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

    jmp 0x20000


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
    dw 0x0000           ; transfer buffer offset -- segment:offset = 0x2000:0x0000 = 0x20000,
    dw 0x2000           ; transfer buffer segment    see this file's own "fix #3" comment
    dq 1                ; starting LBA (sector right after the boot sector)

error_message:
    db "Disk read error!", 0


times 510-($-$$) db 0
dw 0xAA55
