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

erelang_fn_add:
    push rbp
    mov rbp, rsp
    sub rsp, 64
    mov QWORD PTR [rbp-8], rcx
    mov QWORD PTR [rbp-16], rdx
    mov rax, QWORD PTR [rbp-8]
    mov rbx, QWORD PTR [rbp-16]
    add rax, rbx
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    jmp erelang_fn_add_end
erelang_fn_add_end:
    add rsp, 64
    pop rbp
    ret

erelang_fn_main:
    push rbp
    mov rbp, rsp
    sub rsp, 48
    mov rcx, 2
    mov rdx, 3
    call erelang_fn_add
    mov QWORD PTR [rbp-8], rax
    mov rax, QWORD PTR [rbp-8]
    mov QWORD PTR [rbp-16], rax
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-16]
    call printf
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 48
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "sum:%lld\n"
.LG0: .asciz "%lld\\n"
