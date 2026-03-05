.intel_syntax noprefix
.text
.globl main
.extern puts
.extern printf
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
    sub rsp, 48
    sub rsp, 48
    mov rax, 5
    mov QWORD PTR [rsp+32], rax
    mov rax, 6
    mov QWORD PTR [rsp+40], rax
    mov rcx, 1
    mov rdx, 2
    mov r8, 3
    mov r9, 4
    call erelang_fn_sum6
    add rsp, 48
    mov QWORD PTR [rbp-8], rax
    mov rax, QWORD PTR [rbp-8]
    mov QWORD PTR [rbp-16], rax
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-16]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 48
    pop rbp
    ret

erelang_fn_sum6:
    push rbp
    mov rbp, rsp
    sub rsp, 128
    mov QWORD PTR [rbp-8], rcx
    mov QWORD PTR [rbp-16], rdx
    mov QWORD PTR [rbp-24], r8
    mov QWORD PTR [rbp-32], r9
    mov rax, QWORD PTR [rbp+48]
    mov QWORD PTR [rbp-40], rax
    mov rax, QWORD PTR [rbp+56]
    mov QWORD PTR [rbp-48], rax
    mov rax, QWORD PTR [rbp-8]
    mov r10, QWORD PTR [rbp-16]
    add rax, r10
    mov QWORD PTR [rbp-56], rax
    mov rax, QWORD PTR [rbp-56]
    mov r10, QWORD PTR [rbp-24]
    add rax, r10
    mov QWORD PTR [rbp-64], rax
    mov rax, QWORD PTR [rbp-64]
    mov r10, QWORD PTR [rbp-32]
    add rax, r10
    mov QWORD PTR [rbp-72], rax
    mov rax, QWORD PTR [rbp-72]
    mov r10, QWORD PTR [rbp-40]
    add rax, r10
    mov QWORD PTR [rbp-80], rax
    mov rax, QWORD PTR [rbp-80]
    mov r10, QWORD PTR [rbp-48]
    add rax, r10
    mov QWORD PTR [rbp-88], rax
    mov rax, QWORD PTR [rbp-88]
    jmp erelang_fn_sum6_end
erelang_fn_sum6_end:
    add rsp, 128
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "sum6:%lld\n"
.LG0: .asciz "%lld\\n"
