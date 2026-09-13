bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M27/M28
; SMP Initialization & Multicore Trampoline
; ============================================================

%define ACTOR_DEAD     0
%define ACTOR_READY    1
%define ACTOR_RUNNING  2
%define ACTOR_BLOCKED  3

%define MAX_ACTORS     3
%define MAILBOX_SIZE   8
%define MAX_CAPS       4
%define CAP_EMPTY      0
%define CAP_SEND       1

%define MSG_KEYSTROKE  1
%define MSG_VFS_WRITE  2
%define MSG_VFS_READ   3
%define MSG_VFS_LIST   4
%define MSG_VFS_REPLY  5
%define MSG_SYS_CRASH  6   

%define HW_SENDER      0xFF

%define IDT_BASE            0x11000
%define ALLOC_BITMAP_BASE   0x21000
%define TSS_BASE            0x13000
%define DF_STACK_TOP        0x13000  ; V0.33: dedicated double-fault stack (page 0x12, grows down toward 0x12000)
%define E820_MAP_BASE       0x20000
%define MEM_CAP             0x10000000   ; 256MB safety ceiling for V0.30
%define BITMAP_QWORDS       1024         ; covers up to MEM_CAP with margin

; Explicit 64-bit base to prevent NASM sign-extension crashes
%define APIC_BASE           0x00000000FEE00000


; ============================================================
; KERNEL ENTRY
; ============================================================
start:
    cli
    ; V0.29 FIX: RSP was never initialized anywhere in boot.asm or
    ; here before this point, yet the push/retfq sequence a few lines
    ; below (and every "int"/BIOS call before it in boot.asm) executes
    ; with whatever leftover RSP the CPU happened to carry through
    ; real->protected->long mode. It happened to be a survivable
    ; value under QEMU's current reset state, but that's luck, not
    ; a guarantee. Establish a known-good bootstrap stack immediately,
    ; before anything is pushed.
    mov rsp, 0x80000
    mov rdi, TSS_BASE
    xor eax, eax
    mov ecx, 13                       
    rep stosq
    
    mov rax, TSS_BASE
    mov word [rax + 102], 104         

    mov rax, TSS_BASE
    lea rbx, [rel tss_desc]
    mov word [rbx + 2], ax            
    mov rcx, rax
    shr rcx, 16
    mov byte [rbx + 4], cl            
    mov rcx, rax
    shr rcx, 24
    mov byte [rbx + 7], cl            
    mov rcx, rax
    shr rcx, 32
    mov dword [rbx + 8], ecx          

    lgdt [rel gdt64_ptr]
    
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    mov ax, 0x28
    ltr ax
    ; RSP is already 0x80000 from the top of start: (the far
    ; return above nets to zero net stack change), no need to set it again.

    call clear_screen
    call setup_idt
    lidt [rel idt_descriptor]

    call remap_pic
    call memory_init
    
    call actor_init
    call scheduler_init

    ; Load Page Tables so we can reach the APIC memory
    mov rax, [rel actor_cr3 + 0 * 8]
    mov cr3, rax

    call setup_apic
    
    ; M27: Set BSP core count to 1, then wake up the other cores!
    mov dword [0x7000], 1 
    call setup_smp

    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, actor_message
    mov ah, 0x07
    call print_string

    mov al, 0xFD
    out 0x21, al
    jmp start_actor_runtime


; ============================================================
; START ACTOR RUNTIME
; ============================================================
start_actor_runtime:
    mov rcx, [rel actor_kernel_rsp + 0 * 8]
    mov rax, TSS_BASE
    mov [rax + 4], rcx

    mov rax, [rel actor_cr3 + 0 * 8]
    mov cr3, rax

    xor ax, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [rel actor_rsp + 0 * 8]

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq


; ============================================================
; M25: ATOMIC SPINLOCKS
; ============================================================
acquire_kernel_lock:
    mov al, 1
.spin:
    xchg al, [rel kernel_lock]
    test al, al
    jnz .spin
    ret

release_kernel_lock:
    mov byte [rel kernel_lock], 0
    ret


; ============================================================
; FAULT CONTAINMENT & EXCEPTION HANDLERS
; ============================================================
clear_screen:
    mov rdi, 0xB8000
    mov rax, 0x0720072007200720
    mov ecx, 500
    rep stosq
    ret

; ============================================================
; V0.29 FIX: real screen scrolling.
;
; Previously, both the character-print path and the newline path
; handled reaching the bottom of the screen by resetting cursor_y
; straight back to row 2 -- WITHOUT clearing or shifting anything.
; New (usually shorter) lines were then drawn on top of old ones
; without erasing what didn't get overwritten, producing garbled,
; overlapping text every 23 lines (visible as soon as the shell's
; output scrolled past one screenful).
;
; This shifts rows 3..24 up into rows 2..23 and blanks row 24,
; preserving the 2-line banner at the top permanently.
; ============================================================
scroll_screen:
    push rax
    push rcx
    push rsi
    push rdi
    mov rsi, 0xB8000 + (3 * 160)
    mov rdi, 0xB8000 + (2 * 160)
    mov rcx, (22 * 160) / 8
    rep movsq
    mov rdi, 0xB8000 + (24 * 160)
    mov rax, 0x0720072007200720
    mov rcx, 160 / 8
    rep stosq
    pop rdi
    pop rsi
    pop rcx
    pop rax
    ret

print_string:
.next:
    lodsb
    test al, al
    jz .done
    stosw
    jmp .next
.done:
    ret

print_hex:
    push rcx
    push rbx
    push rax
    mov rcx, 16
    add rdi, 30
.loop:
    mov bl, al
    and bl, 0x0F
    cmp bl, 10
    jl .digit
    add bl, 'A' - 10
    jmp .write
.digit:
    add bl, '0'
.write:
    mov byte [rdi], bl
    mov byte [rdi+1], 0x0C 
    sub rdi, 2
    shr rax, 4
    dec rcx
    jnz .loop
    pop rax
    pop rbx
    pop rcx
    ret

debug_halt:
    hlt
    jmp debug_halt

kill_actor_and_switch:
    ; V0.29 FIX: this used to hold the kernel lock across the call to
    ; kernel_inject_message below -- but that function acquires the
    ; SAME non-reentrant spinlock itself. Since a core can't release
    ; a lock it's still spinning to re-acquire, this was a guaranteed
    ; self-deadlock: whichever core killed the actor would spin here
    ; forever, silently, never sending the MSG_SYS_CRASH notice. On
    ; SMP this was easy to miss because the OTHER core (running the
    ; shell) stayed completely responsive the whole time -- confirmed
    ; by reproducing it, then catching the actual page fault in the
    ; exception log and tracing exactly where execution stalled.
    ; Fixed by releasing the lock before calling a function that
    ; manages its own locking, matching how the keyboard handler
    ; (the only other caller of kernel_inject_message) already does it.
    call acquire_kernel_lock
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_state]
    mov byte [rbx + rax], ACTOR_DEAD
    call release_kernel_lock

    sub rsp, 32
    mov dword [rsp], MSG_SYS_CRASH
    mov dword [rsp+4], eax
    mov qword [rsp+8], 0
    mov qword [rsp+16], 0
    mov qword [rsp+24], 0
    mov rdi, 0
    mov rsi, rsp
    call kernel_inject_message
    add rsp, 32

    call scheduler_next

    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov rsp, [rbx + rax * 8]

    lea rbx, [rel actor_cr3]
    mov rcx, [rbx + rax * 8]
    mov cr3, rcx

    lea rbx, [rel actor_kernel_rsp]
    mov rcx, [rbx + rax * 8]
    mov rax, TSS_BASE
    mov [rax + 4], rcx

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq

; ============================================================
; V0.32: CENTRALIZED EXCEPTION HANDLING & REAL DIAGNOSTICS
;
; Before this version, exc_0/exc_6/exc_8/exc_10 each independently
; poked a handful of hardcoded characters into VGA memory and froze
; the machine on ANY fault -- no vector, no error code, no RIP, no
; way to tell what actually happened. Only exc_13 (#GP) and exc_14
; (#PF) checked whether the fault came from ring 3 and routed to
; fault containment; every other exception just bricked the whole
; VM. And vectors outside that hand-picked set had no IDT entry at
; all, so triggering one would fault through garbage and likely
; triple-fault immediately with zero information.
;
; This replaces all of that with one shared path: a tiny per-vector
; stub (just pushes a dummy error code if the CPU doesn't supply one,
; then the vector number, then jumps here) feeding into one common
; handler that applies the SAME rule everywhere -- ring-3 fault ->
; contain and terminate the actor (exactly as exc_13/exc_14 already
; did); ring-0 fault -> this is a genuine kernel bug, not something
; to blame an actor for, so show real diagnostics instead of a blind
; freeze.
; ============================================================

%macro EXC_STUB_NOERR 1
exc_stub_%+%1:
    push qword 0        ; dummy error code -- keeps the frame shape
                         ; identical to vectors that DO push one
    push qword %1        ; vector number
    jmp exception_common
%endmacro

%macro EXC_STUB_ERR 1
exc_stub_%+%1:
    push qword %1        ; vector number (hardware already pushed the error code)
    jmp exception_common
%endmacro

EXC_STUB_NOERR 0
EXC_STUB_NOERR 1
EXC_STUB_NOERR 2
EXC_STUB_NOERR 3
EXC_STUB_NOERR 4
EXC_STUB_NOERR 5
EXC_STUB_NOERR 6
EXC_STUB_NOERR 7
EXC_STUB_ERR   8
EXC_STUB_NOERR 9
EXC_STUB_ERR   10
EXC_STUB_ERR   11
EXC_STUB_ERR   12
EXC_STUB_ERR   13
EXC_STUB_ERR   14
EXC_STUB_NOERR 16
EXC_STUB_ERR   17
EXC_STUB_NOERR 18
EXC_STUB_NOERR 19
EXC_STUB_NOERR 20

; Stack layout on entry (low to high address):
;   [rsp+0]  vector number   (pushed by the stub)
;   [rsp+8]  error code      (real, or the stub's dummy 0)
;   [rsp+16] RIP
;   [rsp+24] CS
;   [rsp+32] RFLAGS
;   [rsp+40] RSP (old)  -- only present if CPL changed
;   [rsp+48] SS (old)   -- only present if CPL changed
exception_common:
    cli
    mov rax, [rsp + 24]         ; interrupted CS
    cmp rax, 0x1B
    jne .kernel_panic

    ; Ring-3 (actor) fault: contain it exactly as before. This is the
    ; same rule exc_13/exc_14 already applied -- now every registered
    ; vector gets it, not just those two. kill_actor_and_switch
    ; discards the current stack/registers wholesale (it switches to
    ; a different, previously-saved actor's context), so nothing
    ; needs to be preserved here first.
    jmp kill_actor_and_switch

.kernel_panic:
    ; A genuine kernel-mode fault. Not an actor's doing -- show real
    ; diagnostics instead of freezing on a couple of hardcoded bytes.
    mov r8, [rsp + 0]           ; vector
    mov r9, [rsp + 8]           ; error code
    mov r10, [rsp + 16]         ; RIP
    mov r11, [rsp + 24]         ; CS
    call panic_screen

    ; Page faults (#PF, vector 14) get one extra line: CR2, the
    ; faulting linear address -- the old handler showed this too,
    ; and it's the single most useful piece of information for
    ; diagnosing a page fault specifically.
    cmp r8, 14
    jne .panic_done
    mov ah, 0x4F
    lea rsi, [rel msg_panic_cr2]
    mov rdi, 0xB8000 + (6 * 160)
    call print_string
    mov rax, cr2
    mov rdi, 0xB8000 + (6 * 160) + 44
    call print_hex
.panic_done:
    jmp debug_halt

; Prints vector, error code, RIP, and CS clearly to VGA and returns.
; Takes its four values in r8-r11 to keep the call site above simple.
panic_screen:
    push rax
    push rdi
    push rsi

    call clear_screen

    mov ah, 0x4F                ; white text, red background
    lea rsi, [rel msg_panic_title]
    mov rdi, 0xB8000
    call print_string

    mov ah, 0x4F
    lea rsi, [rel msg_panic_vector]
    mov rdi, 0xB8000 + (2 * 160)
    call print_string
    mov rax, r8
    mov rdi, 0xB8000 + (2 * 160) + 40
    call print_hex

    mov ah, 0x4F
    lea rsi, [rel msg_panic_errcode]
    mov rdi, 0xB8000 + (3 * 160)
    call print_string
    mov rax, r9
    mov rdi, 0xB8000 + (3 * 160) + 40
    call print_hex

    mov ah, 0x4F
    lea rsi, [rel msg_panic_rip]
    mov rdi, 0xB8000 + (4 * 160)
    call print_string
    mov rax, r10
    mov rdi, 0xB8000 + (4 * 160) + 40
    call print_hex

    mov ah, 0x4F
    lea rsi, [rel msg_panic_cs]
    mov rdi, 0xB8000 + (5 * 160)
    call print_string
    mov rax, r11
    mov rdi, 0xB8000 + (5 * 160) + 40
    call print_hex

    pop rsi
    pop rdi
    pop rax
    ret




; ============================================================
; MEMORY MANAGER
; ============================================================
; ============================================================
; V0.30: Real physical memory manager.
;
; The old memory_init just assumed 16MB of RAM existed (next_free_page
; starting at 0x100000, alloc_page hard-capped at 0x1000000) -- a
; guess, not something derived from the actual machine. This version
; reads the E820 memory map boot.asm left at E820_MAP_BASE (see the
; comment there for the on-disk layout) and builds the allocator's
; bitmap from the machine's REAL reported memory, correctly leaving
; reserved regions and holes marked as unusable rather than assuming
; one contiguous usable range.
;
; Layout at E820_MAP_BASE:
;   dword magic ('E820' = 0x45383230, 0 if boot.asm's BIOS call failed)
;   dword entry_count
;   entry_count * 24-byte entries: {u64 base, u64 length, u32 type, u32 attr}
;   (type == 1 means usable RAM; anything else is reserved/unusable)
; ============================================================
memory_init:
    ; Default every page in range to "not free" -- only pages inside
    ; a genuine type==1 E820 region get cleared to free below. This
    ; is the safe default: an unrecognized/misparsed region stays
    ; unusable rather than accidentally being handed out.
    mov rdi, ALLOC_BITMAP_BASE
    mov rax, 0xFFFFFFFFFFFFFFFF
    mov ecx, BITMAP_QWORDS
    rep stosq

    mov qword [rel next_free_page], 0x100000
    mov qword [rel mem_top], 0x1000000    ; fallback if E820 data is missing/invalid

    mov eax, dword [E820_MAP_BASE]
    cmp eax, 0x45383230
    jne .use_fallback_range

    mov ecx, dword [E820_MAP_BASE + 4]
    test ecx, ecx
    jz .use_fallback_range

    mov rbx, E820_MAP_BASE + 8
    xor r15, r15                           ; highest usable end seen, becomes mem_top

.scan_entry:
    mov eax, dword [rbx + 16]              ; region type
    cmp eax, 1                             ; USABLE?
    jne .scan_next

    mov r8, [rbx]                          ; region base
    mov r9, [rbx + 8]                      ; region length
    add r9, r8                             ; r9 = region end (exclusive)

    ; Clip the region to [0x100000, MEM_CAP)
    mov rax, 0x100000
    cmp r8, rax
    jae .base_ok
    mov r8, rax
.base_ok:
    mov rax, MEM_CAP
    cmp r9, rax
    jbe .end_ok
    mov r9, rax
.end_ok:
    cmp r8, r9
    jae .scan_next                         ; nothing usable left after clipping

    cmp r9, r15
    jbe .no_new_top
    mov r15, r9
.no_new_top:
    call clear_bitmap_range                ; mark [r8, r9) as free

.scan_next:
    add rbx, 24
    dec ecx
    jnz .scan_entry

    cmp r15, 0x100000
    jbe .use_fallback_range         ; nothing usable found above 1MB -- fall back
    mov [rel mem_top], r15
    jmp .done

.use_fallback_range:
    ; No usable E820 data (or none of it was above 1MB) -- fall back
    ; to the old known-safe 16MB range, but still explicitly mark it
    ; free in the bitmap (the bitmap starts all-"used" above), or
    ; every allocation would wrongly report out-of-memory.
    mov r8, 0x100000
    mov r9, 0x1000000
    call clear_bitmap_range

.done:
    ret

; Clears bitmap bits (marks as FREE) for every whole 4KB page inside
; [r8, r9). Both are aligned inward to page boundaries first so a
; region that isn't page-aligned never causes a partial/adjacent page
; to be incorrectly marked free.
clear_bitmap_range:
    push rax
    push rcx
    push rdx
    push rsi
    push r8
    push r9
    push r11

    add r8, 0xFFF
    and r8, ~0xFFF
    and r9, ~0xFFF
    cmp r8, r9
    jae .cbr_done

.cbr_loop:
    mov rax, r8
    sub rax, 0x100000
    shr rax, 12                ; page index relative to the 1MB base
    mov rdx, rax
    shr rdx, 3                 ; byte offset into the bitmap
    and eax, 7                 ; bit offset within that byte
    mov cl, al
    mov r11, 1
    shl r11, cl
    not r11
    lea rsi, [ALLOC_BITMAP_BASE + rdx]
    and byte [rsi], r11b

    add r8, 4096
    cmp r8, r9
    jb .cbr_loop

.cbr_done:
    pop r11
    pop r9
    pop r8
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret


alloc_page:
    mov rbx, [rel next_free_page]
.find:
    cmp rbx, [rel mem_top]
    jae .out_of_memory
    mov rax, rbx
    sub rax, 0x100000
    shr rax, 12
    mov r8, rax
    shr rax, 3
    mov r9, rax
    mov rax, r8
    and eax, 7
    mov edx, 1
    mov ecx, eax
    shl edx, cl
    mov rsi, ALLOC_BITMAP_BASE
    add rsi, r9
    test byte [rsi], dl
    jnz .next
    or byte [rsi], dl
    mov rax, rbx
    add rbx, 4096
    mov [rel next_free_page], rbx

    mov r10, rax
    push rdi
    push rcx
    mov rdi, rax
    xor eax, eax
    mov ecx, 512
    rep stosq
    pop rcx
    pop rdi
    mov rax, r10
    ret
.next:
    add rbx, 4096
    mov [rel next_free_page], rbx
    jmp .find
.out_of_memory:
    xor eax, eax
    ret


; ============================================================
; ADDRESS-SPACE ISOLATION 
; ============================================================
create_address_space:
    push rbp
    mov rbp, rsp
    push r12
    push r13
    push r14
    push r15
    push rdi

    call alloc_page
    mov r14, rax   ; PML4
    call alloc_page
    mov r15, rax   ; PDP
    call alloc_page
    mov r12, rax   ; PD (0-1GB)
    call alloc_page
    mov r13, rax   ; PT (0-2MB)

    mov rax, r15
    or rax, 7
    mov [r14], rax

    mov rax, r12
    or rax, 7
    mov [r15], rax

    mov rax, r13
    or rax, 7
    mov [r12], rax

    push rdi
    call alloc_page
    mov r11, rax
    pop rdi
    
    mov rax, r11
    or rax, 7
    mov [r15 + 3 * 8], rax   
    
    mov rax, APIC_BASE
    or rax, 0x9B             
    mov [r11 + 503 * 8], rax

    xor ecx, ecx
.pt_loop:
    mov rax, rcx
    shl rax, 12
    mov rdx, 7

    ; ------------------------------------------------------------
    ; V0.31: kernel-only structures are locked to supervisor-only
    ; (present + writable, but NOT user-accessible) for EVERY
    ; actor's page table, regardless of actor index -- unlike the
    ; per-actor carve-out checks below, which only apply to ONE
    ; actor's own private region. Kernel code itself is unaffected:
    ; it always runs at CPL=0 through syscalls/interrupts, and
    ; supervisor mode can access supervisor-only pages fine; only
    ; ring-3 (actor) code gets blocked from touching these.
    ; ------------------------------------------------------------
    cmp rcx, 0x8
    jb .not_kernel_fixed
    cmp rcx, 0xA
    jbe .supervisor_only    ; boot's original transient page tables
.not_kernel_fixed:
    cmp rcx, 0x12
    je .supervisor_only     ; V0.33: dedicated double-fault stack
    cmp rcx, IDT_BASE >> 12
    je .supervisor_only
    cmp rcx, TSS_BASE >> 12
    je .supervisor_only
    cmp rcx, E820_MAP_BASE >> 12
    je .supervisor_only
    cmp rcx, ALLOC_BITMAP_BASE >> 12
    jb .not_bitmap
    cmp rcx, (ALLOC_BITMAP_BASE + BITMAP_QWORDS * 8 - 1) >> 12
    jbe .supervisor_only
.not_bitmap:
    mov r10, kernel_private_data_start
    shr r10, 12
    cmp rcx, r10
    jb .check_guards
    mov r10, kernel_private_data_end - 1
    shr r10, 12
    cmp rcx, r10
    jbe .supervisor_only

    ; ------------------------------------------------------------
    ; V0.33: guard pages below each actor's kernel stack.
    ;
    ; Each actor's kernel stack (used while running syscalls/
    ; interrupts on that actor's behalf) is a bare 16KB region with
    ; nothing marking where it ends. Before this version, overflowing
    ; one silently corrupted whatever sits below it -- which, given
    ; how tightly actor_kernel_rsp packs these regions (0x90000,
    ; 0x94000, 0x98000, only 16KB apart), means the NEXT actor's own
    ; kernel stack. No fault, no diagnostic, just quiet corruption
    ; until something else breaks mysteriously later.
    ;
    ; Marking the page immediately below each actor's stack region
    ; not-present turns that into an immediate, diagnosable page
    ; fault (vector 14, CR2 pointing exactly at the guard page --
    ; V0.32's panic screen shows this clearly) the moment a stack
    ; overflow actually happens, instead of silent corruption.
    ;
    ; This is deliberately per-actor, like the carve-out checks
    ; above, not a blanket lockdown: guard_0 (0x8B) is only "not
    ; present" in actor 0's OWN table -- in actor 1's table, that
    ; same page is perfectly ordinary commons memory, since it's
    ; nowhere near actor 1's own stack.
    ; ------------------------------------------------------------
.check_guards:
    cmp rcx, 0x8B
    je .check_guard_0
    cmp rcx, 0x8F
    je .check_guard_1
    cmp rcx, 0x93
    je .check_guard_2
    jmp .check_carveouts

.check_guard_0:
    cmp qword [rsp], 0
    jne .map_it
    xor rdx, rdx
    jmp .map_it
.check_guard_1:
    cmp qword [rsp], 1
    jne .map_it
    xor rdx, rdx
    jmp .map_it
.check_guard_2:
    cmp qword [rsp], 2
    jne .map_it
    xor rdx, rdx
    jmp .map_it

.check_carveouts:
    cmp rcx, 0x6C
    jb .map_it
    cmp rcx, 0x6F
    jbe .check_actor_0
    cmp rcx, 0x70
    jb .map_it
    cmp rcx, 0x73
    jbe .check_actor_1
    cmp rcx, 0x74
    jb .map_it
    cmp rcx, 0x77
    jbe .check_actor_2
    jmp .map_it
.check_actor_0:
    cmp qword [rsp], 0
    je .map_it
    xor rdx, rdx
    jmp .map_it
.check_actor_1:
    cmp qword [rsp], 1
    je .map_it
    xor rdx, rdx
    jmp .map_it
.check_actor_2:
    cmp qword [rsp], 2
    je .map_it
    xor rdx, rdx
    jmp .map_it
.supervisor_only:
    mov rdx, 3               ; present + writable, US bit left clear
.map_it:
    test rdx, rdx
    jz .write_entry
    or rax, rdx
.write_entry:
    mov [r13 + rcx * 8], rax
    inc rcx
    cmp rcx, 512
    jb .pt_loop

    mov rax, r14
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    ret


; ============================================================
; ACTOR RUNTIME INIT
; ============================================================
actor_init:
    mov byte [rel actor_state + 0], ACTOR_READY
    mov byte [rel actor_state + 1], ACTOR_READY
    mov byte [rel actor_state + 2], ACTOR_DEAD 

    mov byte [rel capability_table + 0], CAP_SEND
    mov byte [rel capability_table + 1], 1

    mov byte [rel capability_table + 8], CAP_SEND
    mov byte [rel capability_table + 9], 0

    mov byte [rel mailbox_tail], 0
    mov byte [rel mailbox_count], 0
    ret

scheduler_init:
    mov byte [rel current_actor], 0

    mov rdi, 0
    call create_address_space
    mov [rel actor_cr3 + 0 * 8], rax

    mov rdi, 1
    call create_address_space
    mov [rel actor_cr3 + 1 * 8], rax

    mov rdi, 2
    call create_address_space
    mov [rel actor_cr3 + 2 * 8], rax

    mov rdi, 0x70000
    mov rsi, actor_zero
    mov rdx, 0
    mov r8,  0x90000
    call build_actor_frame

    mov rdi, 0x74000
    mov rsi, actor_one
    mov rdx, 1
    mov r8,  0x94000
    call build_actor_frame
    ret

scheduler_next:
    call acquire_kernel_lock
    movzx eax, byte [rel current_actor]
    mov ecx, MAX_ACTORS
.check:
    inc eax
    cmp eax, MAX_ACTORS
    jb .no_wrap
    xor eax, eax
.no_wrap:
    lea rbx, [rel actor_state]
    mov dl, [rbx + rax]
    cmp dl, ACTOR_READY
    je .found
    cmp dl, ACTOR_RUNNING
    je .found
    dec ecx
    jnz .check

    call release_kernel_lock
    sti
    hlt
    cli
    call acquire_kernel_lock
    mov ecx, MAX_ACTORS
    jmp .check
.found:
    mov [rel current_actor], al
    call release_kernel_lock
    ret


; ============================================================
; ACTOR 0: THE SHELL (RING 3)
; ============================================================
actor_zero:
.init:
    sub rsp, 32
    mov rbp, rsp
    mov dword [rel shell_buffer_idx], 0
    mov byte [rel trace_mode], 0
.prompt:
    mov rsi, 'V'
    mov eax, 3
    int 0x80
    mov rsi, 'a'
    mov eax, 3
    int 0x80
    mov rsi, 'j'
    mov eax, 3
    int 0x80
    mov rsi, 'r'
    mov eax, 3
    int 0x80
    mov rsi, 'a'
    mov eax, 3
    int 0x80
    mov rsi, '>'
    mov eax, 3
    int 0x80
    mov rsi, ' '
    mov eax, 3
    int 0x80

.loop:
    mov rdi, rbp     
    mov eax, 2       
    int 0x80
    test rax, rax
    jz .loop
    
    mov eax, dword [rbp]
    
    cmp eax, MSG_SYS_CRASH
    jne .check_keystroke
    lea rsi, [rel msg_crash_notice]
    call print_string_sys
    jmp .prompt

.check_keystroke:
    cmp eax, MSG_KEYSTROKE
    jne .loop

    movzx r12, byte [rbp + 16]
    
    mov rsi, r12
    mov eax, 3       
    int 0x80

    cmp r12, 10      ; Enter
    je .execute
    cmp r12, 8       ; Backspace
    je .backspace

    mov ebx, dword [rel shell_buffer_idx]
    cmp ebx, 63
    jae .loop        
    lea rdi, [rel shell_buffer]
    mov [rdi + rbx], r12b
    inc ebx
    mov dword [rel shell_buffer_idx], ebx
    jmp .loop

.backspace:
    mov ebx, dword [rel shell_buffer_idx]
    test ebx, ebx
    jz .loop
    dec ebx
    mov dword [rel shell_buffer_idx], ebx
    lea rdi, [rel shell_buffer]
    mov byte [rdi + rbx], 0
    jmp .loop

.execute:
    mov ebx, dword [rel shell_buffer_idx]
    lea rdi, [rel shell_buffer]
    mov byte [rdi + rbx], 0

    mov rsi, rdi
    lea rdx, [rel cmd_help]
    call string_compare
    test rax, rax
    jnz .do_help

    mov rsi, rdi
    lea rdx, [rel cmd_clear]
    call string_compare
    test rax, rax
    jnz .do_clear

    mov rsi, rdi
    lea rdx, [rel cmd_trace]
    call string_compare
    test rax, rax
    jnz .do_trace

    mov rsi, rdi
    lea rdx, [rel cmd_spawn]
    call string_compare
    test rax, rax
    jnz .do_spawn_cmd

    mov rsi, rdi
    lea rdx, [rel cmd_crash]
    call string_compare
    test rax, rax
    jnz .do_crash_cmd

    mov rsi, rdi
    lea rdx, [rel cmd_sysinfo]
    call string_compare
    test rax, rax
    jnz .do_sysinfo_cmd

    mov rsi, rdi
    lea rdx, [rel cmd_ls]
    call string_compare
    test rax, rax
    jnz .do_ls

    mov rsi, rdi
    lea rdx, [rel cmd_read]
    call string_prefix_compare
    test rax, rax
    jnz .do_read

    mov rsi, rdi
    lea rdx, [rel cmd_write]
    call string_prefix_compare
    test rax, rax
    jnz .do_write

    cmp byte [rdi], 0
    je .reset
    call print_unknown
    jmp .reset

.do_help:
    lea rsi, [rel msg_help_text]
    call print_string_sys
    jmp .reset

.do_clear:
    mov eax, 4       
    int 0x80
    jmp .reset

.do_trace:
    mov al, [rel trace_mode]
    xor al, 1
    mov [rel trace_mode], al
    test al, al
    jnz .t_on
    lea rsi, [rel msg_trace_off]
    jmp .t_prt
.t_on:
    lea rsi, [rel msg_trace_on]
.t_prt:
    call print_string_sys
    jmp .reset

.do_spawn_cmd:
    cmp byte [rel trace_mode], 1
    jne .s_skip
    lea rsi, [rel msg_tr_spw]
    call print_string_sys
.s_skip:
    mov eax, 5       
    int 0x80
    jmp .wait_reply

.do_crash_cmd:
    cmp byte [rel trace_mode], 1
    jne .c_skip
    lea rsi, [rel msg_tr_crsh]
    call print_string_sys
.c_skip:
    mov eax, 7       
    int 0x80
    jmp .reset

; M28: Sysinfo now reports real-time Active Cores from hardware
.do_sysinfo_cmd:
    mov eax, 0
    cpuid
    mov dword [rel shell_buffer], ebx
    mov dword [rel shell_buffer + 4], edx
    mov dword [rel shell_buffer + 8], ecx
    mov byte [rel shell_buffer + 12], 10  
    mov byte [rel shell_buffer + 13], 0   

    lea rsi, [rel msg_cpu_vendor]
    call print_string_sys
    lea rsi, [rel shell_buffer]
    call print_string_sys

    lea rsi, [rel msg_cores]
    call print_string_sys

    ; Read atomic core counter from 0x7000
    mov eax, dword [0x7000]
    add al, '0'
    mov byte [rel shell_buffer], al
    mov byte [rel shell_buffer + 1], 10
    mov byte [rel shell_buffer + 2], 0
    lea rsi, [rel shell_buffer]
    call print_string_sys

    ; V0.31: mem_top is now kernel-supervisor-only memory -- fetch it
    ; through a syscall (eax=8) instead of dereferencing it directly,
    ; since a direct read from ring 3 would now page-fault.
    lea rsi, [rel msg_mem_top]
    call print_string_sys

    mov eax, 8
    int 0x80
    xor rdx, rdx
    mov rcx, 0x100000
    div rcx                     ; rax = detected RAM in whole MB

    lea rbx, [rel shell_buffer]
    add rbx, 15
    mov byte [rbx], 0
.mt_digit:
    dec rbx
    xor rdx, rdx
    mov rcx, 10
    div rcx
    add dl, '0'
    mov [rbx], dl
    test rax, rax
    jnz .mt_digit

    mov rsi, rbx
    call print_string_sys

    lea rsi, [rel msg_mem_unit]
    call print_string_sys

    jmp .reset

.do_ls:
    mov dword [rbp], MSG_VFS_LIST
    call trace_send
    mov rdi, 0
    mov rsi, rbp
    mov eax, 1
    int 0x80
    jmp .wait_reply

.do_read:
    mov al, [rel shell_buffer + 5]
    mov [rbp + 16], al
    mov qword [rbp + 17], 0
    mov qword [rbp + 24], 0
    mov dword [rbp], MSG_VFS_READ
    call trace_send
    mov rdi, 0
    mov rsi, rbp
    mov eax, 1
    int 0x80
    jmp .wait_reply

.do_write:
    mov al, [rel shell_buffer + 6]
    mov [rbp + 16], al
    mov qword [rbp + 17], 0
    mov qword [rbp + 24], 0
    mov rcx, 8
    lea rsi, [rel shell_buffer + 8]
    lea rdi, [rbp + 17]
.copy_w:
    mov al, [rsi]
    test al, al
    jz .send_w
    mov [rdi], al
    inc rsi
    inc rdi
    dec rcx
    jnz .copy_w

.send_w:
    mov dword [rbp], MSG_VFS_WRITE
    call trace_send
    mov rdi, 0
    mov rsi, rbp
    mov eax, 1
    int 0x80
    jmp .wait_reply

.wait_reply:
    mov rdi, rbp
    mov eax, 2
    int 0x80
    test rax, rax
    jz .wait_reply

    mov eax, dword [rbp]
    cmp eax, MSG_VFS_REPLY
    jne .check_sys_crash_wait

    call trace_recv

    mov rsi, 10
    mov eax, 3
    int 0x80

    mov rcx, 16
    lea rbx, [rbp + 16]
.print_rep:
    movzx rsi, byte [rbx]
    test rsi, rsi
    jz .rep_done
    push rcx
    push rbx
    mov eax, 3
    int 0x80
    pop rbx
    pop rcx
    inc rbx
    dec rcx
    jnz .print_rep

.rep_done:
    mov rsi, 10
    mov eax, 3
    int 0x80
    jmp .reset

.check_sys_crash_wait:
    cmp eax, MSG_SYS_CRASH
    jne .wait_reply
    lea rsi, [rel msg_crash_notice]
    call print_string_sys
    jmp .reset

.reset:
    mov dword [rel shell_buffer_idx], 0
    jmp .prompt

trace_send:
    cmp byte [rel trace_mode], 1
    jne .done
    push rax
    push rdi
    push rsi
    lea rsi, [rel msg_tr_snd]
    call print_string_sys
    lea rsi, [rel msg_tr_vfs]
    call print_string_sys
    pop rsi
    pop rdi
    pop rax
.done:
    ret

trace_recv:
    cmp byte [rel trace_mode], 1
    jne .done
    push rax
    push rdi
    push rsi
    lea rsi, [rel msg_tr_rcv]
    call print_string_sys
    pop rsi
    pop rdi
    pop rax
.done:
    ret

string_compare:
    push rbx
    push rcx
.cmp_loop:
    mov bl, [rsi]
    mov cl, [rdx]
    cmp bl, cl
    jne .not_match
    test bl, bl
    jz .match
    inc rsi
    inc rdx
    jmp .cmp_loop
.match:
    mov rax, 1
    pop rcx
    pop rbx
    ret
.not_match:
    xor rax, rax
    pop rcx
    pop rbx
    ret

string_prefix_compare:
    push rbx
    push rcx
.p_cmp_loop:
    mov cl, [rdx]
    test cl, cl
    jz .p_match
    mov bl, [rsi]
    cmp bl, cl
    jne .p_not_match
    inc rsi
    inc rdx
    jmp .p_cmp_loop
.p_match:
    mov rax, 1
    pop rcx
    pop rbx
    ret
.p_not_match:
    xor rax, rax
    pop rcx
    pop rbx
    ret

print_unknown:
    lea rsi, [rel msg_unknown]
    jmp print_string_sys

print_string_sys:
    push r12
.ps_loop:
    movzx r12, byte [rsi]
    test r12, r12
    jz .ps_done
    push rsi
    mov rsi, r12
    mov eax, 3
    int 0x80
    pop rsi
    inc rsi
    jmp .ps_loop
.ps_done:
    pop r12
    ret


; ============================================================
; ACTOR 1: THE STORAGE ACTOR (VFS)
; ============================================================
actor_one:
    sub rsp, 32
    mov rbp, rsp
.loop:
    mov rdi, rbp
    mov eax, 2
    int 0x80
    test rax, rax
    jz .loop

    mov eax, dword [rbp]
    cmp eax, MSG_VFS_WRITE
    je .handle_write
    cmp eax, MSG_VFS_READ
    je .handle_read
    cmp eax, MSG_VFS_LIST
    je .handle_list
    jmp .loop

.handle_write:
    mov al, [rbp + 16] 
    lea rbx, [rel ramdisk_files]
    mov ecx, 4
    xor edx, edx
.w_find:
    mov ah, [rbx + rdx]
    test ah, ah
    jz .w_save
    cmp ah, al
    je .w_save
    inc edx
    dec ecx
    jnz .w_find
    
    mov rax, [rel msg_full]
    mov [rbp+16], rax
    mov qword [rbp+24], 0
    jmp .send_reply

.w_save:
    mov [rbx + rdx], al
    mov rax, [rbp + 17]
    lea rbx, [rel ramdisk_data]
    mov [rbx + rdx * 8], rax
    mov rax, [rel msg_ok]
    mov [rbp+16], rax
    mov qword [rbp+24], 0
    jmp .send_reply

.handle_read:
    mov al, [rbp + 16]
    lea rbx, [rel ramdisk_files]
    mov ecx, 4
    xor edx, edx
.r_find:
    mov ah, [rbx + rdx]
    cmp ah, al
    je .r_found
    inc edx
    dec ecx
    jnz .r_find

    mov rax, [rel msg_nofile]
    mov [rbp+16], rax
    mov qword [rbp+24], 0
    jmp .send_reply

.r_found:
    lea rbx, [rel ramdisk_data]
    mov rax, [rbx + rdx * 8]
    mov [rbp+16], rax
    mov qword [rbp+24], 0
    jmp .send_reply

.handle_list:
    mov eax, dword [rel ramdisk_files]
    mov [rbp+16], eax
    mov dword [rbp+20], 0
    mov qword [rbp+24], 0
    jmp .send_reply

.send_reply:
    mov dword [rbp], MSG_VFS_REPLY
    mov rdi, 0 
    mov rsi, rbp
    mov eax, 1 
    int 0x80
    jmp .loop


; ============================================================
; ACTOR 2: THE GHOST WORKER
; ============================================================
ghost_worker:
    mov rcx, 0x0FFFFFFF
.spin:
    dec rcx
    jnz .spin

    sub rsp, 32
    mov rbp, rsp
    mov dword [rbp], MSG_VFS_REPLY 
    mov dword [rbp+4], 2           
    mov qword [rbp+8], 0
    ; V0.29 FIX: this was `mov rax, [rel msg_gh_done]`, which
    ; DEREFERENCES the label and loads its first 8 raw bytes as a
    ; number (0x006F4A20747300... etc, i.e. the ASCII of "Ghost Jo")
    ; instead of the string's address. The receiver here doesn't
    ; treat this field as a pointer anyway -- it prints up to 16
    ; bytes directly from the message body -- so the fix is to
    ; actually copy the message's bytes in, not load-and-store one
    ; qword of the string's own contents. msg_gh_done is defined
    ; below as exactly 16 bytes (padded with nulls) so both qwords
    ; here are meaningful.
    lea rsi, [rel msg_gh_done]
    mov rax, [rsi]
    mov [rbp+16], rax
    mov rax, [rsi+8]
    mov [rbp+24], rax
    
    mov rdi, 0                     
    mov rsi, rbp
    mov eax, 1                     
    int 0x80

    mov eax, 6                     
    int 0x80

kamikaze_worker:
    mov rax, 0x70000
    mov rbx, [rax] 
    mov eax, 6
    int 0x80


; ============================================================
; BUILD ACTOR INTERRUPT FRAME
; ============================================================
build_actor_frame:
    mov rax, r8     
    sub rax, 160    
    mov rcx, rax
    xor r9d, r9d
.clear_gprs:
    mov qword [rcx], 0
    add rcx, 8
    inc r9d
    cmp r9d, 15
    jb .clear_gprs
    mov [rcx], rsi         
    add rcx, 8
    mov qword [rcx], 0x1B  
    add rcx, 8
    mov qword [rcx], 0x202 
    add rcx, 8
    mov [rcx], rdi         
    add rcx, 8
    mov qword [rcx], 0x23  
    mov r9, rdx
    shl r9, 3
    lea rcx, [rel actor_rsp]
    mov [rcx + r9], rax
    ret


; ============================================================
; SYSCALL / IPC KERNEL LOGIC
; ============================================================
kernel_inject_message:
    call acquire_kernel_lock
    cmp rdi, MAX_ACTORS
    jae .done
    mov rcx, rdi
    mov r8, mailbox_count
    movzx eax, byte [r8 + rcx]
    cmp eax, MAILBOX_SIZE
    jae .done
    mov r8, mailbox_tail
    movzx eax, byte [r8 + rcx]
    mov r9, rcx
    shl r9, 3
    add r9, rax
    shl r9, 5
    lea r8, [rel mailbox_data]
    add r8, r9
    mov r10, [rsi]
    mov [r8], r10
    mov r10, [rsi+8]
    mov [r8+8], r10
    mov r10, [rsi+16]
    mov [r8+16], r10
    mov r10, [rsi+24]
    mov [r8+24], r10
    inc al
    and al, MAILBOX_SIZE - 1
    mov r8, mailbox_tail
    mov [r8 + rcx], al
    mov r8, mailbox_count
    inc byte [r8 + rcx]
    lea rbx, [rel actor_state]
    cmp byte [rbx + rcx], ACTOR_BLOCKED
    jne .done
    mov byte [rbx + rcx], ACTOR_READY
.done:
    call release_kernel_lock
    ret

send_message:
    call acquire_kernel_lock
    cmp rdi, MAX_CAPS
    jae .failure
    movzx eax, byte [rel current_actor]
    mov rcx, rax
    shl rcx, 2
    add rcx, rdi
    shl rcx, 1
    lea r8, [rel capability_table]
    add r8, rcx
    cmp byte [r8], CAP_SEND
    jne .failure
    movzx ecx, byte [r8 + 1]
    mov r8, mailbox_count
    movzx eax, byte [r8 + rcx]
    cmp eax, MAILBOX_SIZE
    jae .failure
    mov r8, mailbox_tail
    movzx eax, byte [r8 + rcx]
    mov r9, rcx
    shl r9, 3
    add r9, rax
    shl r9, 5
    lea r8, [rel mailbox_data]
    add r8, r9
    mov r10, [rsi]
    mov [r8], r10
    mov r10, [rsi+8]
    mov [r8+8], r10
    mov r10, [rsi+16]
    mov [r8+16], r10
    mov r10, [rsi+24]
    mov [r8+24], r10
    inc al
    and al, MAILBOX_SIZE - 1
    mov r8, mailbox_tail
    mov [r8 + rcx], al
    mov r8, mailbox_count
    inc byte [r8 + rcx]
    lea rbx, [rel actor_state]
    cmp byte [rbx + rcx], ACTOR_BLOCKED
    jne .skip_wake
    mov byte [rbx + rcx], ACTOR_READY
.skip_wake:
    mov eax, 1
    call release_kernel_lock
    ret
.failure:
    xor eax, eax
    call release_kernel_lock
    ret

receive_message:
    call acquire_kernel_lock
    movzx ecx, byte [rel current_actor]
    mov r8, mailbox_count
    cmp byte [r8 + rcx], 0
    je .empty
    mov r8, mailbox_head
    movzx eax, byte [r8 + rcx]
    mov r9, rcx
    shl r9, 3
    add r9, rax
    shl r9, 5
    lea r8, [rel mailbox_data]
    add r8, r9
    mov r10, [r8]
    mov [rdi], r10
    mov r10, [r8+8]
    mov [rdi+8], r10
    mov r10, [r8+16]
    mov [rdi+16], r10
    mov r10, [r8+24]
    mov [rdi+24], r10
    mov r8, mailbox_head
    movzx edx, byte [r8 + rcx]
    inc dl
    and dl, MAILBOX_SIZE - 1
    mov [r8 + rcx], dl
    mov r8, mailbox_count
    dec byte [r8 + rcx]
    mov eax, 1
    call release_kernel_lock
    ret
.empty:
    lea rbx, [rel actor_state]
    mov byte [rbx + rcx], ACTOR_BLOCKED
    xor eax, eax
    call release_kernel_lock
    ret


; ============================================================
; UNIFIED CONTEXT SWITCH & SYSCALLS
; ============================================================
syscall_handler:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    cmp eax, 1
    je .do_send
    cmp eax, 2
    je .do_recv
    cmp eax, 3
    je .do_print
    cmp eax, 4
    je .do_clear_sys
    cmp eax, 5
    je .do_spawn_sys
    cmp eax, 6
    je .do_exit_sys
    cmp eax, 7
    je .do_spawn_kami_sys
    cmp eax, 8
    je .do_get_memtop
    xor eax, eax
    jmp .done

.do_send:
    call send_message
    jmp .done

.do_recv:
    call receive_message
    mov [rsp + 14 * 8], rax
    test eax, eax
    jz context_switch
    jmp .done

.do_print:
    call acquire_kernel_lock
    cmp sil, 8
    je .backspace
    cmp sil, 10
    je .newline
    movzx eax, byte [rel cursor_y]
    imul eax, 80
    movzx edx, byte [rel cursor_x]
    add eax, edx
    shl eax, 1
    mov rdi, 0xB8000
    add rdi, rax
    mov [rdi], sil
    mov byte [rdi+1], 0x0A 
    inc dl
    cmp dl, 80
    jb .save_x
    xor dl, dl
    inc byte [rel cursor_y]
    cmp byte [rel cursor_y], 25
    jb .save_x
    call scroll_screen
    mov byte [rel cursor_y], 24
.save_x:
    mov [rel cursor_x], dl
    call release_kernel_lock
    jmp .done

.backspace:
    mov dl, [rel cursor_x]
    test dl, dl
    jz .bk_done
    dec dl
    mov [rel cursor_x], dl
    movzx eax, byte [rel cursor_y]
    imul eax, 80
    add eax, edx
    shl eax, 1
    mov rdi, 0xB8000
    add rdi, rax
    mov byte [rdi], ' '
.bk_done:
    call release_kernel_lock
    jmp .done

.newline:
    mov byte [rel cursor_x], 0
    inc byte [rel cursor_y]
    cmp byte [rel cursor_y], 25
    jb .nl_done
    call scroll_screen
    mov byte [rel cursor_y], 24
.nl_done:
    call release_kernel_lock
    jmp .done

.do_clear_sys:
    call acquire_kernel_lock
    mov rdi, 0xB8000 + (2 * 160)
    mov rax, 0x0720072007200720
    mov ecx, 460
    rep stosq
    mov byte [rel cursor_x], 0
    mov byte [rel cursor_y], 2
    call release_kernel_lock
    jmp .done

.do_spawn_sys:
    call acquire_kernel_lock
    cmp byte [rel actor_state + 2], ACTOR_DEAD
    jne .spawn_fail
    mov rdi, 0x78000
    lea rsi, [rel ghost_worker]
    mov rdx, 2
    mov r8, 0x98000
    call build_actor_frame
    mov byte [rel capability_table + 16], CAP_SEND
    mov byte [rel capability_table + 17], 0
    mov byte [rel actor_state + 2], ACTOR_READY
    mov rax, 1
    call release_kernel_lock
    jmp .done
.spawn_fail:
    xor eax, eax
    call release_kernel_lock
    jmp .done

.do_spawn_kami_sys:
    call acquire_kernel_lock
    cmp byte [rel actor_state + 2], ACTOR_DEAD
    jne .spawn_kami_fail
    mov rdi, 0x78000
    lea rsi, [rel kamikaze_worker]
    mov rdx, 2
    mov r8, 0x98000
    call build_actor_frame
    mov byte [rel actor_state + 2], ACTOR_READY
    mov rax, 1
    call release_kernel_lock
    jmp .done
.spawn_kami_fail:
    xor eax, eax
    call release_kernel_lock
    jmp .done

.do_exit_sys:
    call acquire_kernel_lock
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_state]
    mov byte [rbx + rax], ACTOR_DEAD
    call release_kernel_lock
    jmp context_switch

; V0.31: mem_top now lives in the supervisor-only kernel data block
; (see the section comment near kernel_private_data_start), so it's
; no longer directly readable from ring 3 -- sysinfo has to ask for
; it through a syscall like everything else actors need from the
; kernel, instead of dereferencing kernel state directly.
.do_get_memtop:
    mov rax, [rel mem_top]
    jmp .done

.done:
    mov [rsp + 14 * 8], rax
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq


; ============================================================
; KEYBOARD DRIVER (IRQ 1)
; ============================================================
keyboard_handler:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    xor eax, eax
    in al, 0x60
    test al, 0x80
    jnz .eoi

    lea rbx, [rel scancode_map]
    mov cl, [rbx + rax]
    test cl, cl
    jz .eoi

    sub rsp, 32
    mov dword [rsp], MSG_KEYSTROKE   
    mov dword [rsp+4], HW_SENDER     
    mov qword [rsp+8], 0             
    mov qword [rsp+16], 0            
    mov qword [rsp+24], 0
    mov byte [rsp+16], cl            
    
    mov rdi, 0                       
    mov rsi, rsp                     
    call kernel_inject_message
    add rsp, 32

.eoi:
    mov al, 0x20
    out 0x20, al

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq


; ============================================================
; M26/M27: LOCAL APIC & SMP WAKEUP
; ============================================================
setup_apic:
    mov ecx, 0x1B
    rdmsr
    or ah, 0x08     
    wrmsr

    mov rdi, APIC_BASE
    
    mov eax, [rdi + 0xF0]
    or eax, 0x1FF 
    mov [rdi + 0xF0], eax

    mov dword [rdi + 0x3E0], 0x03       
    mov dword [rdi + 0x320], 32 | 0x20000 
    mov dword [rdi + 0x380], 0x100000   
    ret

setup_smp:
    ; 1. Copy 16-bit trampoline to 0x8000
    mov rsi, trampoline_start
    mov rdi, 0x8000
    mov rcx, trampoline_end - trampoline_start
    rep movsb

    ; 2. Broadcast INIT IPI (All Excluding Self)
    mov rax, APIC_BASE
    mov dword [rax + 0x300], 0x000C4500
    
    ; Hardware Delay
    mov rcx, 0x100000
.delay1: 
    dec rcx
    jnz .delay1

    ; 3. Broadcast SIPI IPI (Vector 0x08 -> jumps APs to 0x8000)
    mov dword [rax + 0x300], 0x000C4608
    
    mov rcx, 0x100000
.delay2: 
    dec rcx
    jnz .delay2
    ret

; The 16-bit code the Application Processors will execute upon waking up
align 16
bits 16
trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    lock inc dword [0x7000] ; Atomically increment the core counter
.halt_ap:
    hlt
    jmp .halt_ap
trampoline_end:
bits 64


; ============================================================
; SYSTEM HARDWARE INIT
; ============================================================
setup_idt:
    mov rdi, IDT_BASE
    xor eax, eax
    mov ecx, 512
    rep stosq

    ; V0.33: 4th (optional, default 0) parameter selects an IST
    ; entry -- see the double-fault registration below for why this
    ; matters. IST=0 means "don't switch stacks", the same as before.
    %macro SET_IDT 3-4 0
    lea rax, [rel %1]
    mov rdi, IDT_BASE + %2 * 16
    mov word [rdi], ax
    mov word [rdi + 2], 0x08
    mov byte [rdi + 4], %4
    mov byte [rdi + 5], %3
    shr rax, 16
    mov word [rdi + 6], ax
    shr rax, 16
    mov dword [rdi + 8], eax
    mov dword [rdi + 12], 0
    %endmacro

    SET_IDT exc_stub_0,  0,  10001110b
    SET_IDT exc_stub_1,  1,  10001110b
    SET_IDT exc_stub_2,  2,  10001110b
    SET_IDT exc_stub_3,  3,  10001110b
    SET_IDT exc_stub_4,  4,  10001110b
    SET_IDT exc_stub_5,  5,  10001110b
    SET_IDT exc_stub_6,  6,  10001110b
    SET_IDT exc_stub_7,  7,  10001110b
    ; V0.33: double fault gets its own dedicated stack (IST1) instead
    ; of using whatever RSP was active when it fired. Without this, a
    ; kernel stack overflow (the exact thing this version's guard
    ; pages are meant to catch) escalates past a clean #PF: the CPU's
    ; attempt to push the #PF exception frame onto the SAME already-
    ; broken stack itself faults, escalating to #DF -- and without an
    ; IST here too, THAT delivery attempt fails the same way,
    ; escalating again to a triple fault and a silent CPU reset with
    ; zero diagnostics. Confirmed this exact failure mode by testing:
    ; before this fix, a deliberate stack overflow reset the machine
    ; instead of showing the panic screen (QEMU logged "CPU Reset"
    ; twice -- its signature for a triple fault).
    SET_IDT exc_stub_8,  8,  10001110b, 1
    SET_IDT exc_stub_9,  9,  10001110b
    SET_IDT exc_stub_10, 10, 10001110b
    SET_IDT exc_stub_11, 11, 10001110b
    SET_IDT exc_stub_12, 12, 10001110b
    SET_IDT exc_stub_13, 13, 10001110b
    SET_IDT exc_stub_14, 14, 10001110b
    SET_IDT exc_stub_16, 16, 10001110b
    SET_IDT exc_stub_17, 17, 10001110b
    SET_IDT exc_stub_18, 18, 10001110b
    SET_IDT exc_stub_19, 19, 10001110b
    SET_IDT exc_stub_20, 20, 10001110b
    
    SET_IDT timer_handler, 32, 10001110b
    SET_IDT keyboard_handler, 33, 10001110b 
    SET_IDT syscall_handler, 128, 11101110b 

    ; V0.33: point TSS.IST1 at the top of the dedicated double-fault
    ; stack, so the #DF handler (registered with IST=1 above) always
    ; gets a known-good stack regardless of what RSP was doing when
    ; the double fault fired.
    mov rax, TSS_BASE
    mov qword [rax + 36], DF_STACK_TOP

    ret

remap_pic:
    mov al, 0x11
    out 0x20, al
    call io_wait
    out 0xA0, al
    call io_wait
    mov al, 0x20
    out 0x21, al
    call io_wait
    mov al, 0x28
    out 0xA1, al
    call io_wait
    mov al, 0x04
    out 0x21, al
    call io_wait
    mov al, 0x02
    out 0xA1, al
    call io_wait
    mov al, 0x01
    out 0x21, al
    call io_wait
    out 0xA1, al
    call io_wait
    mov al, 0xFF
    out 0x21, al
    mov al, 0xFF
    out 0xA1, al
    ret

io_wait:
    out 0x80, al
    ret

setup_pit:
    mov al, 0x36
    out 0x43, al
    mov ax, 11932 
    out 0x40, al
    mov al, ah
    out 0x40, al
    ret

timer_handler:
    push rax
    push rbx
    push rcx
    push rdx
    push rbp
    push rdi
    push rsi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    inc byte [rel tick]
    
    mov rdi, APIC_BASE
    mov dword [rdi + 0xB0], 0

    mov rax, [rsp + 128]  
    cmp rax, 0x08
    je timer_skip_context_switch

context_switch:
    call acquire_kernel_lock
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov [rbx + rax * 8], rsp

    call release_kernel_lock
    call scheduler_next
    call acquire_kernel_lock

    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov rsp, [rbx + rax * 8]

    lea rbx, [rel actor_cr3]
    mov rcx, [rbx + rax * 8]
    mov cr3, rcx

    lea rbx, [rel actor_kernel_rsp]
    mov rcx, [rbx + rax * 8]
    mov rax, TSS_BASE
    mov [rax + 4], rcx

    call release_kernel_lock

timer_skip_context_switch:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rsi
    pop rdi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    pop rax
    iretq


; ============================================================
; V0.31: KERNEL-PRIVATE DATA
;
; Everything from here to kernel_private_data_end is exclusively
; kernel-internal state -- the scheduler lock, the memory allocator,
; every actor's CR3/stack pointers, the capability table, and the
; mailboxes actors communicate through. Actors are meant to touch
; all of this ONLY via syscalls (which run at CPL=0).
;
; Before this version, that was true by convention but not enforced:
; the page-permission scheme only ever distinguished each actor's own
; 16KB private carve-out from "everything else", and marked
; EVERYTHING else -- including all of this -- present+user for every
; actor's page tables. A malicious or buggy actor could, for example,
; directly overwrite its own capability_table entry to grant itself
; a capability it was never issued, or corrupt actor_cr3 to point
; another actor's address space at attacker-controlled page tables --
; entirely bypassing the syscall interface the isolation model
; depends on.
;
; This block is page-aligned (both ends) so create_address_space's
; V0.31 addition can mark every whole page inside it supervisor-only
; (present, writable, not user-accessible) uniformly across every
; actor's page tables, regardless of which actor owns the table
; being built. Kernel code itself is unaffected: it keeps running at
; CPL=0 through syscalls/interrupts regardless of which actor's CR3
; happens to be active, and supervisor mode can always reach
; supervisor-only pages.
;
; NOTE: sysinfo used to read mem_top directly from shell code running
; at CPL=3 (added in V0.30) -- that had to change to a syscall
; (eax=8) once this lockdown went in, since a direct ring-3 read of
; a supervisor-only page now correctly page-faults. Worth remembering
; for any future actor-visible feature that wants to read something
; out of this block: it needs a syscall, not a direct dereference.
; ============================================================
align 4096
kernel_private_data_start:

idt_descriptor:
    dw 256 * 16 - 1
    dq IDT_BASE

kernel_lock:       db 0
next_free_page:    dq 0
mem_top:           dq 0     ; V0.30: real top of usable RAM, set by memory_init from the E820 map

align 8
actor_cr3:        times MAX_ACTORS dq 0 
align 8
actor_rsp:        times MAX_ACTORS dq 0
align 8
actor_kernel_rsp:
    dq 0x90000
    dq 0x94000
    dq 0x98000

current_actor:    db 0
actor_state:      times MAX_ACTORS db 0

align 8
capability_table: times MAX_ACTORS * MAX_CAPS * 2 db 0
align 8
mailbox_head:     times MAX_ACTORS db 0
align 8
mailbox_tail:     times MAX_ACTORS db 0
align 8
mailbox_count:    times MAX_ACTORS db 0
align 8
mailbox_data:     times MAX_ACTORS * MAILBOX_SIZE * 4 dq 0

align 8
gdt64:
    dq 0x0000000000000000       
    dq 0x00209A0000000000       ; 0x08 Ring 0 Code
    dq 0x0000920000000000       ; 0x10 Ring 0 Data
    dq 0x0020FA0000000000       ; 0x1B Ring 3 Code
    dq 0x0000F20000000000       ; 0x23 Ring 3 Data

tss_desc:
    dw 103                      
    dw 0                        
    db 0                        
    db 0x89                     
    db 0                        
    db 0                        
    dd 0                        
    dd 0                        
gdt64_end:
gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64

align 4096
kernel_private_data_end:

; ============================================================
; DATA (actor/UI-visible -- low impact if an actor could write here,
; unlike the block above, so this stays in the regular commons)
; ============================================================
tick:             db 0
cursor_x:         db 0
cursor_y:         db 2 
trace_mode:       db 0

message:
    db "VAJRA KERNEL [Version 0.33.0] - Guarded Stacks", 0
actor_message:
    db "Trampoline has successfully booted all sleeping silicon.", 0

; CLI Built-in Strings
cmd_help:         db "help", 0
cmd_clear:        db "clear", 0
cmd_trace:        db "trace", 0
cmd_spawn:        db "spawn", 0
cmd_crash:        db "crash", 0
cmd_sysinfo:      db "sysinfo", 0
cmd_ls:           db "ls", 0
cmd_read:         db "read ", 0
cmd_write:        db "write ", 0

msg_help_text:    db 10, "Cmds: sysinfo, ls, read [f], write [f] [d], trace, spawn, crash", 10, 0
msg_unknown:      db 10, "Unknown command. Try 'help'.", 10, 0
msg_trace_on:     db 10, "[FABRIC] Trace Mode ON", 10, 0
msg_trace_off:    db 10, "[FABRIC] Trace Mode OFF", 10, 0

msg_tr_snd:       db 10, "[FABRIC] Actor 0 ---> [CAP IPC] ---> Actor 1", 10, 0
msg_tr_vfs:       db "[FABRIC] Actor 1 processing block memory...", 10, 0
msg_tr_rcv:       db "[FABRIC] Actor 1 ---> [CAP IPC] ---> Actor 0", 10, 0
msg_tr_spw:       db 10, "[FABRIC] Syscall: Spawn Ephemeral Actor (Slot 2)", 10, 0
msg_tr_crsh:      db 10, "[FABRIC] Syscall: Spawn Malicious Actor (Slot 2)", 10, 0
msg_crash_notice: db 10, "[FABRIC] FAULT CONTAINED: Offending Actor Terminated!", 10, 0

; V0.32: kernel-mode panic diagnostics (see exception_common / panic_screen)
msg_panic_title:   db "KERNEL PANIC - Unhandled Exception (ring 0)", 0
msg_panic_vector:  db "Vector:", 0
msg_panic_errcode: db "Error Code:", 0
msg_panic_rip:     db "RIP:", 0
msg_panic_cs:      db "CS:", 0
msg_panic_cr2:     db "CR2 (fault address):", 0
msg_cpu_vendor:   db 10, "CPU Vendor: ", 0
msg_cores:        db "Active Hardware Cores: ", 0
msg_mem_top:      db "Detected RAM: ", 0
msg_mem_unit:     db " MB", 10, 0

msg_ok:           db "Saved OK"
msg_nofile:       db "No File!"
msg_full:         db "VFS Full"
; V0.29 FIX: shortened to fit the message payload's actual 16-byte
; capacity (see ghost_worker above) -- "Ghost Job Complete &
; Terminated" (32 chars) could never have fit, regardless of the
; load/store bug also fixed at the same time. Padded to exactly 16
; bytes so the two-qword copy above always copies defined bytes.
msg_gh_done:      db "Ghost Job Done", 0, 0

ramdisk_files:    times 4 db 0
ramdisk_data:     times 32 db 0
shell_buffer_idx: dd 0
shell_buffer:     times 64 db 0

scancode_map:
    db 0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', 8
    db 9, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', 10
    db 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', 39, '`'
    db 0, '\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0
    db '*', 0, ' ', 0
    times 128 - ($ - scancode_map) db 0