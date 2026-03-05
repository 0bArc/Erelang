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

erelang_fn_helper:
    push rbp
    mov rbp, rsp
    sub rsp, 48
    mov QWORD PTR [rbp-8], rcx
    mov rax, QWORD PTR [rbp-8]
    mov r10, 1
    add rax, r10
    mov QWORD PTR [rbp-16], rax
    mov rax, QWORD PTR [rbp-16]
    jmp erelang_fn_helper_end
erelang_fn_helper_end:
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
L_for_cond_0:
    mov rax, QWORD PTR [rbp-16]
    mov r10, 5
    cmp rax, r10
    mov rax, 0
    setl al
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    cmp rax, 0
    je L_for_end_3
L_for_body_1:
    sub rsp, 32
    mov rcx, QWORD PTR [rbp-16]
    call erelang_fn_helper
    add rsp, 32
    mov QWORD PTR [rbp-32], rax
    mov rax, QWORD PTR [rbp-8]
    mov r10, QWORD PTR [rbp-32]
    add rax, r10
    mov QWORD PTR [rbp-40], rax
    mov rax, QWORD PTR [rbp-40]
    mov QWORD PTR [rbp-8], rax
L_for_step_2:
    mov rax, QWORD PTR [rbp-16]
    mov r10, 1
    add rax, r10
    mov QWORD PTR [rbp-48], rax
    mov rax, QWORD PTR [rbp-48]
    mov QWORD PTR [rbp-16], rax
    jmp L_for_cond_0
L_for_end_3:
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-8]
    call printf
    add rsp, 32
    jmp L_try_end_5
L_try_catch_4:
    lea rax, .LC1[rip]
    mov QWORD PTR [rbp-56], rax
    sub rsp, 32
    lea rcx, .LC2[rip]
    mov rdx, QWORD PTR [rbp-56]
    call printf
    add rsp, 32
L_try_end_5:
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 96
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "sum:%lld\n"
.LC1: .asciz "native-exception"
.LC2: .asciz "caught:%s\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
