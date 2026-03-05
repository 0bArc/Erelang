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
    lea rcx, .Lmsg3[rip]
    call puts
    lea rcx, .Lmsg4[rip]
    call puts
    lea rcx, .Lmsg5[rip]
    call puts
    lea rcx, .Lmsg6[rip]
    call puts
    xor eax, eax
    add rsp, 32
    pop rbp
    ret

.section .rdata,"dr"
.Lmsg0: .asciz "while:0"
.Lmsg1: .asciz "while:1"
.Lmsg2: .asciz "while:2"
.Lmsg3: .asciz "do-once"
.Lmsg4: .asciz "repeat"
.Lmsg5: .asciz "repeat"
.Lmsg6: .asciz "two"
