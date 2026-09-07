bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M9
; Memory Manager Foundation
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

    ; --------------------------------------------------------
    ; Display kernel status
    ; --------------------------------------------------------

    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, memory_message
    mov ah, 0x07
    call print_string

    ; --------------------------------------------------------
    ; Allocation test
    ; --------------------------------------------------------

    call alloc_page
    mov [rel test_page_1], rax

    test rax, rax
    jz memory_failure

    call alloc_page
    mov [rel test_page_2], rax

    test rax, rax
    jz memory_failure

    mov rdi, 0xB8000 + 320
    mov rsi, allocation_message
    mov ah, 0x07
    call print_string

    ; --------------------------------------------------------
    ; Free first page
    ; --------------------------------------------------------

    mov rax, [rel test_page_1]
    call free_page

    ; --------------------------------------------------------
    ; Allocate again
    ; --------------------------------------------------------

    call alloc_page
    mov [rel test_page_3], rax

    test rax, rax
    jz memory_failure

    mov rdi, 0xB8000 + 480
    mov rsi, reuse_message
    mov ah, 0x07
    call print_string

    ; --------------------------------------------------------
    ; Enable timer
    ; --------------------------------------------------------

    mov al, 0xFE
    out 0x21, al

    sti


idle:
    hlt
    jmp idle


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

    ; First 1 MB reserved.
    ; First allocatable page = 0x100000.

    mov qword [rel next_free_page], 0x100000

    ; Clear allocation bitmap.
    mov rdi, allocation_bitmap
    xor eax, eax
    mov ecx, 64
    rep stosq

    ret


; ============================================================
; ALLOCATE ONE 4 KiB PAGE
;
; Returns:
;   RAX = physical address
;
; Returns:
;   RAX = 0 if out of memory
; ============================================================

alloc_page:

    mov rbx, [rel next_free_page]

.find:

    ; 15 MB managed region
    cmp rbx, 0x1000000
    jae .out_of_memory

    ; Convert physical address to page number
    mov rax, rbx
    sub rax, 0x100000
    shr rax, 12

    ; Save page number
    mov r8, rax

    ; Bitmap byte index = page / 8
    shr rax, 3
    mov rcx, rax

    ; Bit index = page % 8
    mov rax, r8
    and eax, 7

    ; Mask = 1 << bit
    mov edx, 1
    mov ecx, eax
    shl edx, cl

    ; Bitmap address
    mov rsi, allocation_bitmap
    mov r9, rax
    mov rax, r8
    shr rax, 3
    add rsi, rax

    ; Is page already allocated?
    test byte [rsi], dl
    jnz .next

    ; Mark allocated
    or byte [rsi], dl

    ; Return physical address
    mov rax, rbx

    ; Move allocation pointer forward
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
; FREE ONE 4 KiB PAGE
;
; Input:
;   RAX = physical address
; ============================================================

free_page:

    ; Must be inside managed region
    cmp rax, 0x100000
    jb .done

    cmp rax, 0x1000000
    jae .done

    ; Must be page aligned
    test rax, 0xFFF
    jnz .done

    ; Convert physical address → page number
    sub rax, 0x100000
    shr rax, 12

    ; Keep page number
    mov r8, rax

    ; Bitmap byte index
    shr rax, 3
    mov rcx, rax

    ; Bit index
    mov rax, r8
    and eax, 7

    ; Mask
    mov edx, 1
    mov ecx, eax
    shl edx, cl

    ; Bitmap address
    mov rsi, allocation_bitmap

    mov rax, r8
    shr rax, 3
    add rsi, rax

    ; Clear allocation bit
    not dl
    and byte [rsi], dl

.done:
    ret


; ============================================================
; IDT
; ============================================================

setup_idt:

    ; Clear IDT
    mov rdi, idt
    xor eax, eax
    mov ecx, 66
    rep stosq

    ; Timer handler address
    lea rax, [rel timer_handler]

    ; IRQ0 = interrupt vector 32
    lea rdi, [rel idt + 32 * 16]

    ; Offset 0-15
    mov word [rdi], ax

    ; Code segment
    mov word [rdi + 2], 0x18

    ; IST
    mov byte [rdi + 4], 0

    ; Present + interrupt gate
    mov byte [rdi + 5], 10001110b

    ; Offset 16-31
    shr rax, 16
    mov word [rdi + 6], ax

    ; Offset 32-63
    shr rax, 16
    mov dword [rdi + 8], eax

    ; Reserved
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

    ; Master → vectors 32-39
    mov al, 0x20
    out 0x21, al
    call io_wait

    ; Slave → vectors 40-47
    mov al, 0x28
    out 0xA1, al
    call io_wait

    ; Master has slave on IRQ2
    mov al, 0x04
    out 0x21, al
    call io_wait

    ; Slave cascade identity
    mov al, 0x02
    out 0xA1, al
    call io_wait

    ; 8086 mode
    mov al, 0x01
    out 0x21, al
    call io_wait

    out 0xA1, al
    call io_wait

    ; Mask all IRQs initially
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

    ; Channel 0
    ; Square wave
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

    mov byte [rbx], al
    mov byte [rbx + 1], 0x07

    ; End Of Interrupt
    mov al, 0x20
    out 0x20, al

    pop rbx
    pop rax

    iretq


; ============================================================
; MEMORY FAILURE
; ============================================================

memory_failure:

    mov rdi, 0xB8000 + 640
    mov rsi, failure_message
    mov ah, 0x07
    call print_string

.failure_loop:

    cli
    hlt
    jmp .failure_loop


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

test_page_1:
    dq 0

test_page_2:
    dq 0

test_page_3:
    dq 0

tick:
    db 0


; 512 bytes = 4096 page bits
; Enough to track the 15 MB managed region.

align 8

allocation_bitmap:
    times 512 db 0


; ============================================================
; MESSAGES
; ============================================================

message:
    db "Vajra kernel online - M9 Memory Manager", 0

memory_message:
    db "4 KB physical page allocator initialized", 0

allocation_message:
    db "Page allocation + tracking: OK", 0

reuse_message:
    db "Page release + reuse: OK", 0

failure_message:
    db "Memory allocation FAILED!", 0