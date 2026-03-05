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
    xor eax, eax
    add rsp, 32
    pop rbp
    ret

.section .rdata,"dr"
.Lmsg0: .asciz "bootstrap stage ok"
.Lmsg1: .asciz "step:lex"
.Lmsg2: .asciz "step:parse"
.Lmsg3: .asciz "step:ir"
.Lmsg4: .asciz "step:codegen"
.Lmsg5: .asciz "12"
