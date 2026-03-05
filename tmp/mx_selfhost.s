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
    sub rsp, 96
    mov rax, 7
    mov QWORD PTR [rbp-8], rax
    mov rax, 5
    mov QWORD PTR [rbp-16], rax
    mov rax, QWORD PTR [rbp-8]
    mov r10, QWORD PTR [rbp-16]
    add rax, r10
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    mov QWORD PTR [rbp-32], rax
    mov rax, QWORD PTR [rbp-32]
    mov r10, 12
    cmp rax, r10
    mov rax, 0
    sete al
    mov QWORD PTR [rbp-40], rax
    mov rax, QWORD PTR [rbp-40]
    cmp rax, 0
    je L_if_else_0
    sub rsp, 32
    lea rcx, .LC0[rip]
    call puts
    add rsp, 32
    jmp L_if_end_1
L_if_else_0:
L_if_end_1:
    # unresolved call target: list_new
    mov QWORD PTR [rbp-48], rax
    mov rax, QWORD PTR [rbp-48]
    mov QWORD PTR [rbp-56], rax
    lea rax, .LC1[rip]
    mov QWORD PTR [rbp-64], rax
    sub rsp, 32
    lea rcx, .LC5[rip]
    mov rdx, QWORD PTR [rbp-64]
    call printf
    add rsp, 32
    lea rax, .LC2[rip]
    mov QWORD PTR [rbp-64], rax
    sub rsp, 32
    lea rcx, .LC5[rip]
    mov rdx, QWORD PTR [rbp-64]
    call printf
    add rsp, 32
    lea rax, .LC3[rip]
    mov QWORD PTR [rbp-64], rax
    sub rsp, 32
    lea rcx, .LC5[rip]
    mov rdx, QWORD PTR [rbp-64]
    call printf
    add rsp, 32
    lea rax, .LC4[rip]
    mov QWORD PTR [rbp-64], rax
    sub rsp, 32
    lea rcx, .LC5[rip]
    mov rdx, QWORD PTR [rbp-64]
    call printf
    add rsp, 32
    sub rsp, 32
    lea rcx, .LG0[rip]
    mov rdx, QWORD PTR [rbp-32]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 96
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "bootstrap stage ok"
.LC1: .asciz "lex"
.LC2: .asciz "parse"
.LC3: .asciz "ir"
.LC4: .asciz "codegen"
.LC5: .asciz "step:%s\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
