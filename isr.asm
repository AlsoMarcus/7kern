; 7Kern interrupt entry stubs
bits 64
section .text
extern isr_dispatch

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
    mov rdi, rsp
    cld
    call isr_dispatch
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

; vectors 0-47. Exceptions 8, 10-14, 17, 21, 29, 30 push an error code themselves.
%assign i 0
%rep 48
isr %+ i:
  %if !(i == 8 || (i >= 10 && i <= 14) || i == 17 || i == 21 || i == 29 || i == 30)
    push qword 0
  %endif
    push qword i
    jmp isr_common
  %assign i i+1
%endrep

; spurious / unused vectors
global isr_ignore
isr_ignore:
    iretq

section .data
global isr_table
isr_table:
%assign i 0
%rep 48
    dq isr %+ i
  %assign i i+1
%endrep

section .note.GNU-stack noalloc noexec nowrite progbits
