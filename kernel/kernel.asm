bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M12
; Preemptive Actor Runtime
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

    ; Kernel stack
    mov rsp, 0x90000

    ; Interrupt system
    call setup_idt
    lidt [rel idt_descriptor]

    call remap_pic
    call setup_pit

    ; Memory manager
    call memory_init

    ; Actor runtime
    call actor_init

    ; Scheduler
    call scheduler_init

    ; --------------------------------------------------------
    ; Display
    ; --------------------------------------------------------

    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, actor_message
    mov ah, 0x07
    call print_string

    ; --------------------------------------------------------
    ; Enable timer
    ; --------------------------------------------------------

    mov al, 0xFE
    out 0x21, al

    sti


; ============================================================
; START ACTOR RUNTIME
; ============================================================

start_actor_runtime:

    ; Start Actor 0 from its fabricated interrupt-return frame.
    ; The frame contains 15 saved GPRs followed by RIP/CS/RFLAGS/RSP/SS.
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
; PRINT STRING
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
; MEMORY MANAGER
; ============================================================

memory_init:

    mov qword [rel next_free_page], 0x100000

    mov rdi, allocation_bitmap
    xor eax, eax
    mov ecx, 64
    rep stosq

    ret


; ------------------------------------------------------------
; Allocate one 4 KiB page
; ------------------------------------------------------------

alloc_page:

    mov rbx, [rel next_free_page]

.find:

    cmp rbx, 0x1000000
    jae .out_of_memory

    mov rax, rbx
    sub rax, 0x100000
    shr rax, 12

    mov r8, rax

    ; Bitmap byte
    shr rax, 3
    mov r9, rax

    ; Bit number
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
; ACTOR RUNTIME
; ============================================================

actor_init:

    ; ----------------------------------------
    ; Actor 0
    ; ----------------------------------------

    mov byte [rel actor_state + 0], ACTOR_READY
    mov byte [rel actor_id + 0], 0

    ; ----------------------------------------
    ; Actor 1
    ; ----------------------------------------

    mov byte [rel actor_state + 1], ACTOR_READY
    mov byte [rel actor_id + 1], 1

    ; ----------------------------------------
    ; Actor 2
    ; ----------------------------------------

    mov byte [rel actor_state + 2], ACTOR_READY
    mov byte [rel actor_id + 2], 2

    mov byte [rel actor_count], 3

    ; Clear mailbox metadata
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

    ; Clear mailbox storage
    mov rdi, mailbox_data
    xor eax, eax
    mov ecx, 24
    rep stosq

    ; Give Actor 0 an initial message
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
    ; Each actor has a private 16 KiB stack region.
    ;
    ; The saved RSP points to the first saved GPR (r15).
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

    ; Verify all contexts were initialized.
    mov rax, [rel actor_rsp + 0 * 8]
    test rax, rax
    jz scheduler_init_fault

    mov rax, [rel actor_rsp + 1 * 8]
    test rax, rax
    jz scheduler_init_fault

    mov rax, [rel actor_rsp + 2 * 8]
    test rax, rax
    jz scheduler_init_fault

    ret


scheduler_init_fault:

    mov rdi, 0xB8000 + 800
    mov rsi, scheduler_fault_message
    mov ah, 0x07
    call print_string

.hang:
    cli
    hlt
    jmp .hang


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
; ACTOR ENTRY POINTS
; ============================================================

; These are independently-running actor contexts.
; The timer interrupt preempts them and switches contexts.

actor_zero:

    mov byte [rel actor_state + 0], ACTOR_RUNNING

.loop:

    mov rdi, 0
    call receive_message

    test rax, rax
    jz .loop

    inc byte [rel actor_messages + 0]

    ; Display Actor 0
    mov rdi, 0xB8000 + 320

    mov byte [rdi], 'A'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    ; Send to Actor 1
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

    ; Display Actor 1
    mov rdi, 0xB8000 + 480

    mov byte [rdi], 'B'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    ; Send to Actor 2
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

    ; Display Actor 2
    mov rdi, 0xB8000 + 640

    mov byte [rdi], 'C'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    ; Send back to Actor 0
    mov rdi, 0
    mov rsi, 'A'
    call send_message

    jmp .loop


; ============================================================
; BUILD ACTOR INTERRUPT FRAME
; ============================================================

; Input:
;   RDI = top of actor stack
;   RSI = actor entry RIP
;   RDX = actor ID
;
; Output:
;   actor_rsp[actor_id] = frame base
;
; Frame layout (160 bytes total):
;
;   +00 r15
;   +08 r14
;   +16 r13
;   +24 r12
;   +32 r11
;   +40 r10
;   +48 r9
;   +56 r8
;   +64 rsi
;   +72 rdi
;   +80 rbp
;   +88 rdx
;   +96 rcx
;   +104 rbx
;   +112 rax
;   +120 RIP      (Pushed by hardware on interrupt)
;   +128 CS       (Pushed by hardware on interrupt)
;   +136 RFLAGS   (Pushed by hardware on interrupt)
;   +144 RSP      (Pushed by hardware on 64-bit interrupt)
;   +152 SS       (Pushed by hardware on 64-bit interrupt)

build_actor_frame:

    ; Calculate first slot of the 160-byte synthetic frame.
    mov rax, rdi
    sub rax, 160

    mov rcx, rax

    ; 15 saved GPR slots.
    xor r8d, r8d

.clear_gprs:

    mov qword [rcx], 0

    add rcx, 8

    inc r8d

    cmp r8d, 15
    jb .clear_gprs

    ; Synthetic IRETQ frame
    
    ; 1. RIP
    mov [rcx], rsi
    add rcx, 8

    ; 2. CS
    mov qword [rcx], 0x18
    add rcx, 8

    ; 3. RFLAGS: IF=1
    mov qword [rcx], 0x202
    add rcx, 8

    ; 4. RSP
    mov [rcx], rdi
    add rcx, 8

    ; 5. SS (Data Segment)
    mov qword [rcx], 0x10

    ; Save exact frame base.
    mov r8, rdx
    shl r8, 3

    lea rcx, [rel actor_rsp]

    mov [rcx + r8], rax

    ret


; ============================================================
; SEND MESSAGE
;
; Input:
;   RDI = target actor ID
;   RSI = message value
;
; Returns:
;   RAX = 1 success
;   RAX = 0 failure
; ============================================================

send_message:

    cmp rdi, MAX_ACTORS
    jae .failure

    ; Check mailbox capacity
    mov rax, rdi
    mov rcx, rax

    mov r8, mailbox_count
    movzx eax, byte [r8 + rcx]

    cmp eax, MAILBOX_SIZE
    jae .failure

    ; Get tail position
    mov r8, mailbox_tail
    movzx eax, byte [r8 + rcx]

    ; index = actor * MAILBOX_SIZE + tail
    mov r9, rdi
    shl r9, 3
    add r9, rax

    ; mailbox address
    mov r8, mailbox_data
    mov [r8 + r9 * 8], rsi

    ; Advance tail
    inc al
    and al, MAILBOX_SIZE - 1

    mov r8, mailbox_tail
    mov [r8 + rcx], al

    ; Increase count
    mov r8, mailbox_count
    inc byte [r8 + rcx]

    mov eax, 1

    ret


.failure:

    xor eax, eax

    ret


; ============================================================
; RECEIVE MESSAGE
;
; Input:
;   RDI = actor ID
;
; Returns:
;   RAX = message
;   RAX = 0 if mailbox empty
; ============================================================

receive_message:

    cmp rdi, MAX_ACTORS
    jae .empty

    mov rcx, rdi

    ; Check count
    mov r8, mailbox_count

    cmp byte [r8 + rcx], 0
    je .empty

    ; Head position
    mov r8, mailbox_head
    movzx eax, byte [r8 + rcx]

    ; index = actor * MAILBOX_SIZE + head
    mov r9, rdi
    shl r9, 3
    add r9, rax

    ; Read message
    mov r8, mailbox_data
    mov rax, [r8 + r9 * 8]

    ; Advance head
    mov r8, mailbox_head
    movzx edx, byte [r8 + rcx]

    inc dl
    and dl, MAILBOX_SIZE - 1

    mov [r8 + rcx], dl

    ; Decrease count
    mov r8, mailbox_count
    dec byte [r8 + rcx]

    ret


.empty:

    xor eax, eax

    ret


; ============================================================
; IDT
; ============================================================

setup_idt:

    mov rdi, idt

    xor eax, eax

    mov ecx, 66

    rep stosq

    lea rax, [rel timer_handler]

    ; IRQ0 = vector 32
    lea rdi, [rel idt + 32 * 16]

    mov word [rdi], ax

    mov word [rdi + 2], 0x18

    mov byte [rdi + 4], 0

    mov byte [rdi + 5], 10001110b

    shr rax, 16

    mov word [rdi + 6], ax

    shr rax, 16

    mov dword [rdi + 8], eax

    mov dword [rdi + 12], 0

    ret


; ============================================================
; PIC
; ============================================================

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


; ============================================================
; PIT
; ============================================================

setup_pit:

    mov al, 0x36
    out 0x43, al

    mov ax, 11932

    out 0x40, al

    mov al, ah
    out 0x40, al

    ret


; ============================================================
; TIMER / PREEMPTIVE CONTEXT SWITCH
; ============================================================

timer_handler:

    ; Preserve all general-purpose registers.
    ;
    ; Stack after hardware interrupt pushes (SS, RSP, RFLAGS, CS, RIP)
    ; plus our manual pushes:
    ;
    ;   r15
    ;   r14
    ;   ...
    ;   rax
    ;   hardware RIP
    ;   hardware CS
    ;   hardware RFLAGS
    ;   hardware RSP
    ;   hardware SS
    ;
    ; This exactly matches our updated 160-byte build_actor_frame.

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

    ; Save interrupted actor's context.
    movzx eax, byte [rel current_actor]

    lea rbx, [rel actor_rsp]

    mov [rbx + rax * 8], rsp

    ; Select next actor.
    call scheduler_next

    ; scheduler_next uses RBX internally.
    ; Reload actor_rsp afterwards.
    lea rbx, [rel actor_rsp]

    ; Load selected actor context.
    movzx eax, byte [rel current_actor]

    mov rsp, [rbx + rax * 8]

    ; Send EOI to master PIC.
    mov al, 0x20
    out 0x20, al

    ; Restore selected actor context.
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
; IDT STORAGE
; ============================================================

align 16

idt:

    times 33 * 16 db 0

idt_end:

idt_descriptor:

    dw idt_end - idt - 1
    dq idt


; ============================================================
; MEMORY DATA
; ============================================================

next_free_page:
    dq 0

allocation_bitmap:
    times 512 db 0


; ============================================================
; ACTOR DATA
; ============================================================

align 8

actor_rsp:
    times MAX_ACTORS dq 0

actor_count:
    db 0

current_actor:
    db 0

actor_id:
    times MAX_ACTORS db 0

actor_state:
    times MAX_ACTORS db 0

actor_messages:
    times MAX_ACTORS db 0


; ============================================================
; MAILBOX DATA
;
; 3 actors × 8 messages × 8 bytes
; ============================================================

align 8

mailbox_head:
    times MAX_ACTORS db 0

align 8

mailbox_tail:
    times MAX_ACTORS db 0

align 8

mailbox_count:
    times MAX_ACTORS db 0

align 8

mailbox_data:
    times MAX_ACTORS * MAILBOX_SIZE dq 0


; ============================================================
; GLOBAL DATA
; ============================================================

tick:
    db 0


; ============================================================
; MESSAGES
; ============================================================

message:
    db "Vajra kernel online - M12 Preemptive Actor Runtime", 0

actor_message:
    db "3 actors + timer-driven context switching + mailboxes", 0
scheduler_fault_message:
    db "M12 actor context init FAILED", 0