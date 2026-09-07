bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M10
; Scheduler Foundation
; ============================================================

start:
    cli

    ; Kernel stack
    mov rsp, 0x90000

    ; --------------------------------------------------------
    ; Interrupt system
    ; --------------------------------------------------------

    call setup_idt
    lidt [rel idt_descriptor]

    call remap_pic
    call setup_pit

    ; --------------------------------------------------------
    ; Memory manager
    ; --------------------------------------------------------

    call memory_init

    ; --------------------------------------------------------
    ; Scheduler
    ; --------------------------------------------------------

    call scheduler_init

    ; --------------------------------------------------------
    ; Display startup information
    ; --------------------------------------------------------

    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, scheduler_message
    mov ah, 0x07
    call print_string

    ; --------------------------------------------------------
    ; Enable timer
    ; --------------------------------------------------------

    mov al, 0xFE
    out 0x21, al

    sti


; ============================================================
; MAIN SCHEDULER LOOP
; ============================================================

scheduler_loop:

    ; Wait for timer tick
    mov al, [rel tick]

.wait:
    cmp al, [rel tick]
    je .wait

    ; Select next task
    call scheduler_next

    ; Execute selected task
    call scheduler_run_current

    jmp scheduler_loop


; ============================================================
; STRING OUTPUT
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


; ============================================================
; SIMPLE PAGE ALLOCATOR
; ============================================================

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
    mov rcx, rax

    ; Bit
    mov rax, r8
    and eax, 7

    mov edx, 1
    mov ecx, eax
    shl edx, cl

    mov rsi, allocation_bitmap

    mov rax, r8
    shr rax, 3
    add rsi, rax

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
; SCHEDULER
; ============================================================

scheduler_init:

    ; Three initial kernel tasks
    mov byte [rel task_count], 3

    ; Task 0
    mov byte [rel task_state + 0], 1

    ; Task 1
    mov byte [rel task_state + 1], 1

    ; Task 2
    mov byte [rel task_state + 2], 1

    ; Start with task 0
    mov byte [rel current_task], 0

    ret


; ------------------------------------------------------------
; Select next READY task
; ------------------------------------------------------------

scheduler_next:

    movzx eax, byte [rel current_task]

    inc eax

    cmp eax, 3
    jb .check

    xor eax, eax

.check:

    lea rbx, [rel task_state]
    mov bl, [rbx + rax]

    cmp bl, 1
    je .found

    inc eax

    cmp eax, 3
    jb .check

    xor eax, eax

    ; Fallback
    mov [rel current_task], al

    ret

.found:

    mov [rel current_task], al

    ret


; ------------------------------------------------------------
; Run currently selected task
; ------------------------------------------------------------

scheduler_run_current:

    movzx eax, byte [rel current_task]

    cmp eax, 0
    je task_zero

    cmp eax, 1
    je task_one

    cmp eax, 2
    je task_two

    ret


; ============================================================
; TASK 0
; ============================================================

task_zero:

    inc byte [rel task_counter_0]

    movzx eax, byte [rel task_counter_0]
    and eax, 0x0F

    cmp al, 10
    jb .digit

    add al, 'A' - 10
    jmp .display

.digit:

    add al, '0'

.display:

    mov rdi, 0xB8000 + 320

    mov byte [rdi], 'A'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    ret


; ============================================================
; TASK 1
; ============================================================

task_one:

    inc byte [rel task_counter_1]

    movzx eax, byte [rel task_counter_1]
    and eax, 0x0F

    cmp al, 10
    jb .digit

    add al, 'A' - 10
    jmp .display

.digit:

    add al, '0'

.display:

    mov rdi, 0xB8000 + 480

    mov byte [rdi], 'B'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

    ret


; ============================================================
; TASK 2
; ============================================================

task_two:

    inc byte [rel task_counter_2]

    movzx eax, byte [rel task_counter_2]
    and eax, 0x0F

    cmp al, 10
    jb .digit

    add al, 'A' - 10
    jmp .display

.digit:

    add al, '0'

.display:

    mov rdi, 0xB8000 + 640

    mov byte [rdi], 'C'
    mov byte [rdi + 1], 0x07

    mov byte [rdi + 2], ':'
    mov byte [rdi + 3], 0x07

    mov byte [rdi + 4], al
    mov byte [rdi + 5], 0x07

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

    ; ~100 Hz
    mov ax, 11932

    out 0x40, al

    mov al, ah
    out 0x40, al

    ret


; ============================================================
; TIMER INTERRUPT
; ============================================================

timer_handler:

    push rax
    push rbx

    inc byte [rel tick]

    ; Display scheduler tick
    movzx eax, byte [rel tick]
    and eax, 0x0F

    cmp al, 10
    jb .digit

    add al, 'A' - 10
    jmp .display

.digit:

    add al, '0'

.display:

    mov rbx, 0xB8000 + 160

    mov byte [rbx], 'T'
    mov byte [rbx + 1], 0x07

    mov byte [rbx + 2], ':'
    mov byte [rbx + 3], 0x07

    mov byte [rbx + 4], al
    mov byte [rbx + 5], 0x07

    ; End Of Interrupt
    mov al, 0x20
    out 0x20, al

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
; SCHEDULER DATA
; ============================================================

task_count:
    db 0

current_task:
    db 0

task_state:
    times 3 db 0

task_counter_0:
    db 0

task_counter_1:
    db 0

task_counter_2:
    db 0


; ============================================================
; GLOBAL DATA
; ============================================================

tick:
    db 0


; ============================================================
; MESSAGES
; ============================================================

message:
    db "Vajra kernel online - M10 Scheduler", 0

scheduler_message:
    db "Round-robin scheduler: 3 tasks", 0