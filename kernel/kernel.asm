bits 64
org 0x1000

; ============================================================
; VAJRA KERNEL - M24
; Fault Containment & Blast Radius Mitigation
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

; Vajra Message Types
%define MSG_KEYSTROKE  1
%define MSG_VFS_WRITE  2
%define MSG_VFS_READ   3
%define MSG_VFS_LIST   4
%define MSG_VFS_REPLY  5
%define MSG_SYS_CRASH  6   

%define HW_SENDER      0xFF

%define IDT_BASE            0x11000
%define ALLOC_BITMAP_BASE   0x12000
%define TSS_BASE            0x13000


; ============================================================
; KERNEL ENTRY
; ============================================================
start:
    cli
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
    mov rsp, 0x80000

    call clear_screen
    call setup_idt
    lidt [rel idt_descriptor]

    call remap_pic
    call setup_pit
    call memory_init
    call actor_init
    call scheduler_init

    mov rdi, 0xB8000
    mov rsi, message
    mov ah, 0x07
    call print_string

    mov rdi, 0xB8000 + 160
    mov rsi, actor_message
    mov ah, 0x07
    call print_string

    mov al, 0xFC
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
; FAULT CONTAINMENT & EXCEPTION HANDLERS
; ============================================================
clear_screen:
    mov rdi, 0xB8000
    mov rax, 0x0720072007200720
    mov ecx, 500
    rep stosq
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
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_state]
    mov byte [rbx + rax], ACTOR_DEAD

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

exc_0:
    cli
    mov rdi, 0xB8000
    mov rax, 0x0421044504440420
    mov [rdi], rax
    jmp debug_halt
exc_6:
    cli
    mov rdi, 0xB8000
    mov rax, 0x0421044404550420
    mov [rdi], rax
    jmp debug_halt
exc_8:
    cli
    mov rdi, 0xB8000
    mov rax, 0x0421044C04420444
    mov [rdi], rax
    mov rax, [rsp]              
    mov rdi, 0xB8000 + 10
    call print_hex
    jmp debug_halt
exc_10:
    cli
    mov rdi, 0xB8000
    mov rax, 0x0421045304530454
    mov [rdi], rax
    mov rax, [rsp]              
    mov rdi, 0xB8000 + 10
    call print_hex
    jmp debug_halt

exc_13:
    cli
    mov rax, [rsp + 16]
    cmp rax, 0x1B
    je kill_actor_and_switch
    mov rdi, 0xB8000
    mov rax, 0x0421044604500447
    mov [rdi], rax
    mov rax, [rsp]              
    mov rdi, 0xB8000 + 10
    call print_hex
    jmp debug_halt

exc_14:
    cli
    mov rax, [rsp + 16]
    cmp rax, 0x1B
    je kill_actor_and_switch
    mov rdi, 0xB8000
    mov rax, 0x0421044604470450 
    mov [rdi], rax
    mov rax, [rsp]              
    mov rdi, 0xB8000 + 10
    call print_hex
    mov rax, cr2                
    mov rdi, 0xB8000 + 50
    call print_hex
    jmp debug_halt


; ============================================================
; MEMORY MANAGER
; ============================================================
memory_init:
    mov qword [rel next_free_page], 0x100000
    mov rdi, ALLOC_BITMAP_BASE
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
    mov r14, rax
    call alloc_page
    mov r15, rax
    call alloc_page
    mov r12, rax
    call alloc_page
    mov r13, rax

    mov rax, r15
    or rax, 7
    mov [r14], rax
    mov rax, r12
    or rax, 7
    mov [r15], rax
    mov rax, r13
    or rax, 7
    mov [r12], rax

    xor ecx, ecx
.pt_loop:
    mov rax, rcx
    shl rax, 12
    mov rdx, 7  

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

    sti
    hlt
    cli
    mov ecx, MAX_ACTORS
    jmp .check
.found:
    mov [rel current_actor], al
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
; ACTOR 2: THE GHOST WORKER (M23/M24)
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
    mov rax, [rel msg_gh_done]
    mov [rbp+16], rax
    mov qword [rbp+24], 0
    
    mov rdi, 0                     
    mov rsi, rbp
    mov eax, 1                     
    int 0x80

    mov eax, 6                     
    int 0x80

kamikaze_worker:
    ; M24: I am a bad actor. I will attempt to read Actor 1's private memory!
    mov rax, 0x70000
    mov rbx, [rax] ; BOOM! Hardware MMU throws Page Fault (#PF) here.
    
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
    ret

send_message:
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
    ret
.failure:
    xor eax, eax
    ret

receive_message:
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
    ret
.empty:
    lea rbx, [rel actor_state]
    mov byte [rbx + rcx], ACTOR_BLOCKED
    xor eax, eax
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
    mov byte [rel cursor_y], 2
.save_x:
    mov [rel cursor_x], dl
    jmp .done

.backspace:
    mov dl, [rel cursor_x]
    test dl, dl
    jz .done
    dec dl
    mov [rel cursor_x], dl
    movzx eax, byte [rel cursor_y]
    imul eax, 80
    add eax, edx
    shl eax, 1
    mov rdi, 0xB8000
    add rdi, rax
    mov byte [rdi], ' '
    jmp .done

.newline:
    mov byte [rel cursor_x], 0
    inc byte [rel cursor_y]
    cmp byte [rel cursor_y], 25
    jb .done
    mov byte [rel cursor_y], 2
    jmp .done

.do_clear_sys:
    mov rdi, 0xB8000 + (2 * 160)
    mov rax, 0x0720072007200720
    mov ecx, 460
    rep stosq
    mov byte [rel cursor_x], 0
    mov byte [rel cursor_y], 2
    jmp .done

.do_spawn_sys:
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
    jmp .done
.spawn_fail:
    xor eax, eax
    jmp .done

.do_spawn_kami_sys:
    cmp byte [rel actor_state + 2], ACTOR_DEAD
    jne .spawn_fail
    mov rdi, 0x78000
    lea rsi, [rel kamikaze_worker]
    mov rdx, 2
    mov r8, 0x98000
    call build_actor_frame
    mov byte [rel actor_state + 2], ACTOR_READY
    mov rax, 1
    jmp .done

.do_exit_sys:
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_state]
    mov byte [rbx + rax], ACTOR_DEAD
    jmp context_switch

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
; SYSTEM HARDWARE INIT
; ============================================================
setup_idt:
    mov rdi, IDT_BASE
    xor eax, eax
    mov ecx, 512
    rep stosq

    %macro SET_IDT 3
    lea rax, [rel %1]
    mov rdi, IDT_BASE + %2 * 16
    mov word [rdi], ax
    mov word [rdi + 2], 0x08
    mov byte [rdi + 4], 0
    mov byte [rdi + 5], %3
    shr rax, 16
    mov word [rdi + 6], ax
    shr rax, 16
    mov dword [rdi + 8], eax
    mov dword [rdi + 12], 0
    %endmacro

    SET_IDT exc_0, 0, 10001110b
    SET_IDT exc_6, 6, 10001110b
    SET_IDT exc_8, 8, 10001110b
    SET_IDT exc_10, 10, 10001110b
    SET_IDT exc_13, 13, 10001110b
    SET_IDT exc_14, 14, 10001110b
    
    SET_IDT timer_handler, 32, 10001110b
    SET_IDT keyboard_handler, 33, 10001110b 
    SET_IDT syscall_handler, 128, 11101110b 
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
    mov al, 0x20
    out 0x20, al

    mov rax, [rsp + 128]  
    cmp rax, 0x08
    je timer_skip_context_switch

context_switch:
    movzx eax, byte [rel current_actor]
    lea rbx, [rel actor_rsp]
    mov [rbx + rax * 8], rsp

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
; GDT
; ============================================================
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


; ============================================================
; DATA
; ============================================================
align 16
idt_descriptor:
    dw 256 * 16 - 1
    dq IDT_BASE

next_free_page:    dq 0
allocation_bitmap: times 512 db 0

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

tick:             db 0
cursor_x:         db 0
cursor_y:         db 2 
trace_mode:       db 0

message:
    db "VAJRA KERNEL [Version 0.24.1] - Hardware Fault Containment", 0
actor_message:
    db "Type 'crash' to spawn an illegal memory violator.", 0

; CLI Built-in Strings
cmd_help:         db "help", 0
cmd_clear:        db "clear", 0
cmd_trace:        db "trace", 0
cmd_spawn:        db "spawn", 0
cmd_crash:        db "crash", 0
cmd_ls:           db "ls", 0
cmd_read:         db "read ", 0
cmd_write:        db "write ", 0

msg_help_text:    db 10, "Cmds: ls, read [f], write [f] [d], trace, spawn, crash", 10, 0
msg_unknown:      db 10, "Unknown command. Try 'help'.", 10, 0
msg_trace_on:     db 10, "[FABRIC] Trace Mode ON", 10, 0
msg_trace_off:    db 10, "[FABRIC] Trace Mode OFF", 10, 0

msg_tr_snd:       db 10, "[FABRIC] Actor 0 ---> [CAP IPC] ---> Actor 1", 10, 0
msg_tr_vfs:       db "[FABRIC] Actor 1 processing block memory...", 10, 0
msg_tr_rcv:       db "[FABRIC] Actor 1 ---> [CAP IPC] ---> Actor 0", 10, 0
msg_tr_spw:       db 10, "[FABRIC] Syscall: Spawn Ephemeral Actor (Slot 2)", 10, 0
msg_tr_crsh:      db 10, "[FABRIC] Syscall: Spawn Malicious Actor (Slot 2)", 10, 0
msg_crash_notice: db 10, "[FABRIC] FAULT CONTAINED: Offending Actor Terminated!", 10, 0

msg_ok:           db "Saved OK"
msg_nofile:       db "No File!"
msg_full:         db "VFS Full"
msg_gh_done:      db "Ghost Job Complete & Terminated", 0

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