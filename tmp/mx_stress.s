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

erelang_fn_add:
    push rbp
    mov rbp, rsp
    sub rsp, 64
    mov QWORD PTR [rbp-8], rcx
    mov QWORD PTR [rbp-16], rdx
    mov rax, QWORD PTR [rbp-8]
    mov r10, QWORD PTR [rbp-16]
    add rax, r10
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    jmp erelang_fn_add_end
erelang_fn_add_end:
    add rsp, 64
    pop rbp
    ret

erelang_fn_inc:
    push rbp
    mov rbp, rsp
    sub rsp, 48
    mov QWORD PTR [rbp-8], rcx
    mov rax, QWORD PTR [rbp-8]
    mov r10, 1
    add rax, r10
    mov QWORD PTR [rbp-16], rax
    mov rax, QWORD PTR [rbp-16]
    jmp erelang_fn_inc_end
erelang_fn_inc_end:
    add rsp, 48
    pop rbp
    ret

erelang_fn_main:
    push rbp
    mov rbp, rsp
    sub rsp, 96
    mov rax, 0
    mov QWORD PTR [rbp-8], rax
    mov rax, 0
    mov QWORD PTR [rbp-16], rax
L_while_start_0:
    mov rax, QWORD PTR [rbp-8]
    mov r10, 2000
    cmp rax, r10
    mov rax, 0
    setl al
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    cmp rax, 0
    je L_while_end_1
    sub rsp, 32
    mov rcx, QWORD PTR [rbp-8]
    call erelang_fn_inc
    add rsp, 32
    mov QWORD PTR [rbp-32], rax
    mov rax, QWORD PTR [rbp-32]
    mov QWORD PTR [rbp-40], rax
    sub rsp, 32
    mov rcx, QWORD PTR [rbp-16]
    mov rdx, QWORD PTR [rbp-40]
    call erelang_fn_add
    add rsp, 32
    mov QWORD PTR [rbp-48], rax
    mov rax, QWORD PTR [rbp-48]
    mov QWORD PTR [rbp-16], rax
    mov rax, QWORD PTR [rbp-8]
    mov r10, 1
    add rax, r10
    mov QWORD PTR [rbp-56], rax
    mov rax, QWORD PTR [rbp-56]
    mov QWORD PTR [rbp-8], rax
    jmp L_while_start_0
L_while_end_1:
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-16]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 96
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "acc:%lld\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
