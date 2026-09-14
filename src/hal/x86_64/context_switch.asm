bits 64

; void hal_context_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3)
;
; Classic minimal cooperative switch: since this is reached via a
; normal C call, the caller's CALLER-saved registers are already
; preserved by the compiler around the call site (standard SysV
; AMD64 ABI) -- only the CALLEE-saved registers (rbx, rbp, r12-r15)
; need to be saved explicitly here.
;
; A newly-spawned actor that has never actually run has no real
; suspended context to resume; core/actor.c builds a fake one on its
; stack shaped exactly like what this function's own push sequence
; would have produced, so the final `ret` below lands on the actor's
; real entry point the first time it's switched to.
;
; new_cr3 (rdx, the 3rd SysV integer arg) switches to the target
; actor's own private address space -- see hal/x86_64/paging.c. This
; MUST happen strictly after `mov rsp, rsi` but before the pop
; sequence below: switching the stack pointer itself needs no
; translation (it's just a register value), but the pops right after
; it dereference that address, which is only valid once CR3 actually
; points at a table that maps it. Switching CR3 any earlier, while
; still executing with the OLD rsp, would be fine too, but there is no
; benefit to it and this ordering keeps the "stack pointer" and
; "address space" swaps visibly adjacent.

global hal_context_switch

section .text
hal_context_switch:
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp      ; *old_rsp = current rsp (after the pushes above)
    mov rsp, rsi         ; switch to the new context's stack
    mov cr3, rdx          ; switch to the new context's address space

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    ret                  ; returns into whatever RIP was on the new stack
