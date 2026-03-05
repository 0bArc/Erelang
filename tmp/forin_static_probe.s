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
    # unresolved call target: list_new
    mov QWORD PTR [rbp-8], rax
    mov rax, QWORD PTR [rbp-8]
    mov QWORD PTR [rbp-16], rax
    mov rax, 10
    mov QWORD PTR [rbp-24], rax
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-24]
    call printf
    add rsp, 32
    mov rax, 20
    mov QWORD PTR [rbp-24], rax
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-24]
    call printf
    add rsp, 32
    mov rax, 30
    mov QWORD PTR [rbp-24], rax
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-24]
    call printf
    add rsp, 32
    # unresolved call target: dict_new
    mov QWORD PTR [rbp-32], rax
    mov rax, QWORD PTR [rbp-32]
    mov QWORD PTR [rbp-40], rax
    lea rax, .LC1[rip]
    mov QWORD PTR [rbp-48], rax
    mov rax, 1
    mov QWORD PTR [rbp-24], rax
    sub rsp, 32
    lea rcx, .LC3[rip]
    mov rdx, QWORD PTR [rbp-48]
    mov r8, QWORD PTR [rbp-24]
    call printf
    add rsp, 32
    lea rax, .LC2[rip]
    mov QWORD PTR [rbp-48], rax
    mov rax, 2
    mov QWORD PTR [rbp-24], rax
    sub rsp, 32
    lea rcx, .LC3[rip]
    mov rdx, QWORD PTR [rbp-48]
    mov r8, QWORD PTR [rbp-24]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 80
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "v:%lld\n"
.LC1: .asciz "a"
.LC2: .asciz "b"
.LC3: .asciz "%s:%lld\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
