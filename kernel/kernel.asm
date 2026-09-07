bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M12
; Pure Preemptive Actor Runtime (Ring 0, Shared Address Space)
; ============================================================

%define ACTOR_DEAD     0
%define ACTOR_READY    1
%define ACTOR_RUNNING  2
%define ACTOR_BLOCKED  3

%define MAX_ACTORS     3
%define MAILBOX_SIZE   8


; ============================================================
; KERNEL ENTRY
; ============================================================

start:
    cli

    ; 1. Load M12 Kernel GDT
    lgdt [rel gdt64_ptr]

    ; 2. Reload Data Segments with Ring 0 Data (0x10)
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    ; 3. Reload Code Segment with Ring 0 Code (0x08)
    push 0x08
    lea rax, [rel .reload_cs]
    push rax
    retfq

.reload_cs:
    ; Kernel stack for initialization
    mov rsp, 0x90000

    call setup_idt
    lidt [rel idt_descriptor]

    call remap_pic
    call setup_pit

    call memory_init
    call actor_init
    call scheduler_init

    ; Display
    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, actor_message
    mov ah, 0x07
    call print_string

    ; Enable timer hardware
    mov al, 0xFE
    out 0x21, al

    ; Jump into the preemptive runtime (Never returns here)
    jmp start_actor_runtime


; ============================================================
; START ACTOR RUNTIME
; ============================================================

start_actor_runtime:
    ; Load Actor 0's initial synthetic stack frame
    mov rsp, [rel actor_rsp + 0 * 8]

    ; Restore 15 General Purpose Registers
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

    ; Pops RIP, CS, RFLAGS, RSP, SS and starts executing actor_zero
    iretq


; ============================================================
; DISPLAY
; ============================================================

print_string:
.next:
    lodsb
    test al, al
    jz .done
    stosw
    jmp .next
.done:
    ret


; ============================================================
; MEMORY MANAGER (M9 Baseline)
; ============================================================

memory_init:
    mov qword [rel next_free_page], 0x100000
    mov rdi, allocation_bitmap
    xor eax, eax
    mov ecx, 64
    rep stosq
    ret

alloc_page:
    mov rbx, [rel next_free_page]
.find:
    cmp rbx, 0x1000000
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
    mov rsi, allocation_bitmap
    add rsi, r9
    test byte [rsi], dl
    jnz .next
    or byte [rsi], dl
    mov rax, rbx
    add rbx, 4096
    mov [rel next_free_page], rbx
    ret
.next:
    add rbx, 4096
    mov [rel next_free_page], rbx
    jmp .find
.out_of_memory:
    xor eax, eax
    ret


; ============================================================
; ACTOR RUNTIME INIT (M11 Baseline)
; ============================================================

actor_init:
    mov byte [rel actor_state + 0], ACTOR_READY
    mov byte [rel actor_id + 0], 0

    mov byte [rel actor_state + 1], ACTOR_READY
    mov byte [rel actor_id + 1], 1

    mov byte [rel actor_state + 2], ACTOR_READY
    mov byte [rel actor_id + 2], 2

    mov byte [rel actor_count], 3

    mov rdi, mailbox_head
    xor eax, eax
    mov ecx, 3
    rep stosq

    mov rdi, mailbox_tail
    xor eax, eax
    mov ecx, 3
    rep stosq

    mov rdi, mailbox_count
    xor eax, eax
    mov ecx, 3
    rep stosq

    mov rdi, mailbox_data
    xor eax, eax
    mov ecx, 24
    rep stosq

    ; Inject the first message ('S') to Actor 0
    mov rdi, 0
    mov rsi, 'S'
    call send_message
    ret


; ============================================================
; ACTOR SCHEDULER
; ============================================================

scheduler_init:
    mov byte [rel current_actor], 0

    ; Build one initial interrupt-return frame for each actor.
    mov rdi, 0x70000
    mov rsi, actor_zero
    mov rdx, 0
    call build_actor_frame

    mov rdi, 0x74000
    mov rsi, actor_one
    mov rdx, 1
    call build_actor_frame

    mov rdi, 0x78000
    mov rsi, actor_two
    mov rdx, 2
    call build_actor_frame
    ret

scheduler_next:
    movzx eax, byte [rel current_actor]
    inc eax
    cmp eax, MAX_ACTORS
    jb .check
    xor eax, eax
.check:
    lea rbx, [rel actor_state]
    mov dl, [rbx + rax]
    cmp dl, ACTOR_READY
    je .found
    cmp dl, ACTOR_RUNNING
    je .found
    inc eax
    cmp eax, MAX_ACTORS
    jb .check
    xor eax, eax
.found:
    mov [rel current_actor], al
    ret


; ============================================================
; ACTOR ENTRY POINTS (RING 0 INFINITE LOOPS)
; ============================================================

actor_zero:
    mov byte [rel actor_state + 0], ACTOR_RUNNING
.loop:
    mov rdi, 0
    call receive_message
    test rax, rax
    jz .loop

    inc byte [rel actor_messages + 0]
    mov rdi, 0xB8000 + 320
    mov byte [rdi], 'A'
    mov byte [rdi + 1], 0x07
    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07
    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    mov rdi, 1
    mov rsi, 'B'
    call send_message
    jmp .loop


actor_one:
    mov byte [rel actor_state + 1], ACTOR_RUNNING
.loop:
    mov rdi, 1
    call receive_message
    test rax, rax
    jz .loop

    inc byte [rel actor_messages + 1]
    mov rdi, 0xB8000 + 480
    mov byte [rdi], 'B'
    mov byte [rdi + 1], 0x07
    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07
    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    mov rdi, 2
    mov rsi, 'C'
    call send_message
    jmp .loop


actor_two:
    mov byte [rel actor_state + 2], ACTOR_RUNNING
.loop:
    mov rdi, 2
    call receive_message
    test rax, rax
    jz .loop

    inc byte [rel actor_messages + 2]
    mov rdi, 0xB8000 + 640
    mov byte [rdi], 'C'
    mov byte [rdi + 1], 0x07
    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07
    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    mov rdi, 0
    mov rsi, 'A'
    call send_message
    jmp .loop


; ============================================================
; BUILD ACTOR INTERRUPT FRAME
; ============================================================

; Input: RDI=stack top, RSI=RIP, RDX=actor_id
build_actor_frame:
    mov rax, rdi
    sub rax, 160           ; 160-byte frame (15 GPRs + 5 HW regs)
    mov rcx, rax

    ; Clear 15 General Purpose Registers
    xor r8d, r8d
.clear_gprs:
    mov qword [rcx], 0
    add rcx, 8
    inc r8d
    cmp r8d, 15
    jb .clear_gprs

    ; Synthetic IRETQ frame (Hardware pops these)
    mov [rcx], rsi         ; RIP
    add rcx, 8
    mov qword [rcx], 0x08  ; CS (Ring 0 Code)
    add rcx, 8
    mov qword [rcx], 0x202 ; RFLAGS (Interrupts Enabled)
    add rcx, 8
    mov [rcx], rdi         ; RSP (Actor Stack Base)
    add rcx, 8
    mov qword [rcx], 0x10  ; SS (Ring 0 Data)

    ; Save frame pointer to actor_rsp array
    mov r8, rdx
    shl r8, 3
    lea rcx, [rel actor_rsp]
    mov [rcx + r8], rax
    ret


; ============================================================
; SEND / RECEIVE MESSAGES (M11 Baseline)
; ============================================================

send_message:
    cmp rdi, MAX_ACTORS
    jae .failure
    mov rcx, rdi
    mov r8, mailbox_count
    movzx eax, byte [r8 + rcx]
    cmp eax, MAILBOX_SIZE
    jae .failure
    mov r8, mailbox_tail
    movzx eax, byte [r8 + rcx]
    mov r9, rdi
    shl r9, 3
    add r9, rax
    mov r8, mailbox_data
    mov [r8 + r9 * 8], rsi
    inc al
    and al, MAILBOX_SIZE - 1
    mov r8, mailbox_tail
    mov [r8 + rcx], al
    mov r8, mailbox_count
    inc byte [r8 + rcx]
    mov eax, 1
    ret
.failure:
    xor eax, eax
    ret

receive_message:
    cmp rdi, MAX_ACTORS
    jae .empty
    mov rcx, rdi
    mov r8, mailbox_count
    cmp byte [r8 + rcx], 0
    je .empty
    mov r8, mailbox_head
    movzx eax, byte [r8 + rcx]
    mov r9, rdi
    shl r9, 3
    add r9, rax
    mov r8, mailbox_data
    mov rax, [r8 + r9 * 8]
    mov r8, mailbox_head
    movzx edx, byte [r8 + rcx]
    inc dl
    and dl, MAILBOX_SIZE - 1
    mov [r8 + rcx], dl
    mov r8, mailbox_count
    dec byte [r8 + rcx]
    ret
.empty:
    xor eax, eax
    ret


; ============================================================
; SYSTEM HARDWARE & TIMER IRQ
; ============================================================

setup_idt:
    mov rdi, idt
    xor eax, eax
    mov ecx, 66               ; 33 descriptors * 2 QWORDs each
    rep stosq

    ; Register IRQ0 (Timer) at vector 32
    lea rax, [rel timer_handler]
    lea rdi, [rel idt + 32 * 16]
    mov word [rdi], ax
    mov word [rdi + 2], 0x08  ; Kernel CS
    mov byte [rdi + 4], 0
    mov byte [rdi + 5], 10001110b
    shr rax, 16
    mov word [rdi + 6], ax
    shr rax, 16
    mov dword [rdi + 8], eax
    mov dword [rdi + 12], 0
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

; THIS IS THE HEART OF M12
timer_handler:
    ; 1. Preserve interrupted actor's registers
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

    ; 2. Save preempted actor's RSP
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov [rbx + rax * 8], rsp

    ; 3. Pick next actor
    call scheduler_next

    ; 4. Load next actor's RSP
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov rsp, [rbx + rax * 8]

    ; 5. Acknowledge Interrupt
    mov al, 0x20
    out 0x20, al

    ; 6. Restore next actor's registers
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

    ; 7. Resume execution
    iretq


; ============================================================
; GDT (M12 Minimal Setup)
; ============================================================

align 8
gdt64:
    dq 0x0000000000000000       ; 0x00 Null Descriptor
    dq 0x00209A0000000000       ; 0x08 Ring 0 Code
    dq 0x0000920000000000       ; 0x10 Ring 0 Data
gdt64_end:

gdt64_ptr:
    dw gdt64_end - gdt64 - 1
    dq gdt64


; ============================================================
; DATA
; ============================================================

align 16
idt: times 33 * 16 db 0
idt_end:
idt_descriptor:
    dw idt_end - idt - 1
    dq idt

next_free_page:    dq 0
allocation_bitmap: times 512 db 0

align 8
actor_rsp:        times MAX_ACTORS dq 0

actor_count:      db 0
current_actor:    db 0
actor_id:         times MAX_ACTORS db 0
actor_state:      times MAX_ACTORS db 0
actor_messages:   times MAX_ACTORS db 0

align 8
mailbox_head:     times MAX_ACTORS db 0
align 8
mailbox_tail:     times MAX_ACTORS db 0
align 8
mailbox_count:    times MAX_ACTORS db 0
align 8
mailbox_data:     times MAX_ACTORS * MAILBOX_SIZE dq 0

tick:             db 0

message:
    db "Vajra kernel online - M12 Pure Preemptive Runtime", 0
actor_message:
    db "3 Ring-0 Actors + Timer-Driven Context Switching", 0