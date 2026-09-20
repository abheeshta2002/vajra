bits 64

; ------------------------------------------------------------------
; Tiny per-vector stubs, same design as the assembly kernel's V0.32
; centralized exception handling: vectors without a hardware error
; code get a dummy 0 pushed first so every vector presents the same
; stack shape, then all of them jump to one shared handler.
; ------------------------------------------------------------------

%macro ISR_NOERR 1
global isr_stub_%1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common
%endmacro

%macro ISR_ERR 1
global isr_stub_%1
isr_stub_%1:
    push qword %1
    jmp isr_common
%endmacro

extern exception_handler

section .text

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Stack now: [15 saved regs = 120 bytes][vector][error_code][RIP][CS][RFLAGS]...
    mov rdi, [rsp + 120]    ; 1st C arg: vector
    mov rsi, [rsp + 128]    ; 2nd C arg: error code
    mov rdx, [rsp + 136]    ; 3rd C arg: RIP
    mov rcx, [rsp + 144]    ; 4th C arg: CS -- low 2 bits are the CPL the
                             ; fault actually came from (Phase 23): a
                             ; ring-3 CS here means this is an ACTOR's
                             ; fault, not the kernel's, and shouldn't
                             ; take the whole machine down with it.
    call exception_handler

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16             ; drop vector + error_code
    iretq

ISR_NOERR 0                 ; #DE  divide error
ISR_NOERR 6                 ; #UD  invalid opcode
ISR_ERR   8                 ; #DF  double fault
ISR_ERR   13                ; #GP  general protection
ISR_ERR   14                ; #PF  page fault

; IRQ0 (timer), remapped to vector 32 by hal_pic_remap() -- a hardware
; interrupt with no CPU-supplied error code, exactly like ISR_NOERR's
; exception cases, so the same macro/shared isr_common handles it.
ISR_NOERR 32

; IRQ1 (PS/2 keyboard, roadmap Phase 18), remapped to vector 33 --
; same reasoning as vector 32 above.
ISR_NOERR 33

; IRQ12 (PS/2 mouse, docs/DESKTOP_DESIGN.md Stage 1), remapped to
; vector 44 -- same reasoning as vector 32/33 above.
ISR_NOERR 44

; Phase 10: the local APIC timer (vector 48) -- each non-boot core's own
; preemption tick, since the PIT/8259 pair (vector 32) only ever
; interrupts the BSP -- and the LAPIC's spurious-interrupt vector (255),
; which needs a present gate that simply returns.
ISR_NOERR 48
ISR_NOERR 255

; ------------------------------------------------------------------
; Vector 0x80 (128): the syscall gate (DPL=3 -- see interrupts.c's
; idt_set_gate call for it). Deliberately NOT routed through
; isr_common/exception_handler: a syscall needs its actual arguments
; (RAX = number, RDI/RSI/RDX = up to 3 args), not just vector/
; error_code/RIP, and it returns a value back to the caller in RAX --
; different enough from "report a fault and never come back" that a
; parallel path is clearer than overloading exception_handler's
; signature with syscall-only concerns.
; ------------------------------------------------------------------

extern syscall_handler

global isr_stub_128
isr_stub_128:
    push qword 0            ; dummy error code -- int 0x80 supplies none, same reasoning as ISR_NOERR
    push qword 128
    jmp syscall_common

syscall_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Same stack shape isr_common documents (15 saved regs, then
    ; vector/error_code/RIP/CS/RFLAGS/...), so the same offsets apply:
    ; saved rax sits at rsp+112, rdi at rsp+72, rsi at rsp+80, rdx at
    ; rsp+88. Reading the SAVED copies (not the live registers, which
    ; are about to be clobbered by the call anyway) into the SysV
    ; argument registers for syscall_handler(num, a1, a2, a3).
    mov rdi, [rsp + 112]    ; num  (was rax)
    mov rsi, [rsp + 72]     ; a1   (was rdi)
    mov rdx, [rsp + 80]     ; a2   (was rsi)
    mov rcx, [rsp + 88]     ; a3   (was rdx)
    call syscall_handler
    mov [rsp + 112], rax    ; overwrite saved rax with the return value, so the pop below
                             ; restores it into the caller's rax

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax
    add rsp, 16
    iretq
