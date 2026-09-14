bits 64

; void hal_enter_user_mode(uint64_t entry, uint64_t user_stack_top)
;
; Drops from ring 0 to ring 3 via iretq -- the standard technique:
; iretq always restores CS/SS/RSP/RFLAGS/RIP from the stack, and the
; CPU treats that as an ordinary privilege-level change if the pushed
; CS has RPL 3, exactly as if a real interrupt had originally brought
; us here from ring 3. Building the frame by hand and "returning" into
; it is the standard first entry into user mode for a hand-rolled
; kernel that isn't using an ELF loader.
;
; Never returns: rdi (entry) and rsi (user_stack_top, the 2 SysV
; integer args) are only ever read, before the iretq discards this
; function's own stack frame along with everything else about the
; ring-0 context that called it.
;
; Selectors: GDT_USER_CODE_SEL (0x38 | RPL 3 = 0x3B) and
; GDT_USER_DATA_SEL (0x30 | RPL 3 = 0x33) -- see hal/x86_64/gdt.c.
; Hardcoded here rather than shared via a header because this is the
; only other file that needs them, and because they're tied to gdt.c's
; specific GDT layout (entries 6 and 7) in a way a shared #define
; wouldn't make any safer.

%define USER_CODE_SEL 0x3B
%define USER_DATA_SEL 0x33

global hal_enter_user_mode

section .text
hal_enter_user_mode:
    mov ax, USER_DATA_SEL
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    ; SS is deliberately not loaded here -- iretq sets it from the
    ; frame pushed below, which is the only place that matters.

    push qword USER_DATA_SEL   ; SS
    push rsi                    ; RSP = user_stack_top
    pushfq
    pop rax
    or rax, 0x200                ; force IF=1: this actor must be preemptible once in ring 3
    push rax                     ; RFLAGS
    push qword USER_CODE_SEL   ; CS
    push rdi                    ; RIP = entry
    iretq
