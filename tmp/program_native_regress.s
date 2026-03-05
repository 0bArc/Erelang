.intel_syntax noprefix
.text
.globl main
.extern puts
.extern printf
.extern scanf
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
    sub rsp, 80
    mov rax, 10
    mov QWORD PTR [rbp-8], rax
    lea rax, .LC0[rip]
    mov QWORD PTR [rbp-16], rax
    mov rax, 100
    mov QWORD PTR [rbp-24], rax
    lea rax, .LC1[rip]
    mov QWORD PTR [rbp-32], rax
    # unresolved call target: list_new
    mov QWORD PTR [rbp-40], rax
    mov rax, QWORD PTR [rbp-40]
    mov QWORD PTR [rbp-48], rax
    lea rax, .LC1[rip]
    mov QWORD PTR [rbp-32], rax
    sub rsp, 32
    lea rcx, .LC4[rip]
    mov rdx, QWORD PTR [rbp-32]
    call printf
    add rsp, 32
    lea rax, .LC2[rip]
    mov QWORD PTR [rbp-32], rax
    sub rsp, 32
    lea rcx, .LC4[rip]
    mov rdx, QWORD PTR [rbp-32]
    call printf
    add rsp, 32
    lea rax, .LC3[rip]
    mov QWORD PTR [rbp-32], rax
    sub rsp, 32
    lea rcx, .LC4[rip]
    mov rdx, QWORD PTR [rbp-32]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 80
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "A"
.LC1: .asciz "Alex"
.LC2: .asciz "Adam"
.LC3: .asciz "Bob"
.LC4: .asciz "%s\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
