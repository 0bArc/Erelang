.intel_syntax noprefix
.text
.globl main
.extern puts
.extern printf
.extern scanf
.extern Sleep
.extern getchar
.extern strcmp

main:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    call erelang_fn_main
    add rsp, 32
    pop rbp
    ret

erelang_fn_main:
    push rbp
    mov rbp, rsp
    sub rsp, 32
    sub rsp, 32
    lea rcx, .LC0[rip]
    call puts
    add rsp, 32
    sub rsp, 32
    mov rax, 10
    mov ecx, eax
    call Sleep
    add rsp, 32
    sub rsp, 32
    lea rcx, .LC1[rip]
    call puts
    add rsp, 32
    sub rsp, 32
    lea rcx, .LC2[rip]
    call puts
    add rsp, 32
    sub rsp, 32
    call getchar
    add rsp, 32
    sub rsp, 32
    lea rcx, .LC3[rip]
    call puts
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 32
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "before"
.LC1: .asciz "after-sleep"
.LC2: .asciz "press-enter"
.LC3: .asciz "after-pause"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
