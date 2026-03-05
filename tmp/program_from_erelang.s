.intel_syntax noprefix
.text
.globl main
.extern puts

main:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    lea rcx, .Lmsg0[rip]
    call puts
    lea rcx, .Lmsg1[rip]
    call puts
    lea rcx, .Lmsg2[rip]
    call puts
    xor eax, eax
    add rsp, 32
    pop rbp
    ret

.section .rdata,"dr"
.Lmsg0: .asciz "Alex"
.Lmsg1: .asciz "Adam"
.Lmsg2: .asciz "Bob"
