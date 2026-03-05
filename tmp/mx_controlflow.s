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
    sub rsp, 112
    mov rax, 0
    mov QWORD PTR [rbp-8], rax
L_while_start_0:
    mov rax, QWORD PTR [rbp-8]
    mov r10, 3
    cmp rax, r10
    mov rax, 0
    setl al
    mov QWORD PTR [rbp-16], rax
    mov rax, QWORD PTR [rbp-16]
    cmp rax, 0
    je L_while_end_1
    sub rsp, 32
    lea rcx, .LC0[rip]
    mov rdx, QWORD PTR [rbp-8]
    call printf
    add rsp, 32
    mov rax, QWORD PTR [rbp-8]
    mov r10, 1
    add rax, r10
    mov QWORD PTR [rbp-24], rax
    mov rax, QWORD PTR [rbp-24]
    mov QWORD PTR [rbp-8], rax
    jmp L_while_start_0
L_while_end_1:
L_do_start_2:
    sub rsp, 32
    lea rcx, .LC1[rip]
    call puts
    add rsp, 32
    mov rax, 0
    cmp rax, 0
    jne L_do_start_2
    mov rax, 0
    mov QWORD PTR [rbp-32], rax
L_repeat_loop_3:
    mov rax, QWORD PTR [rbp-32]
    mov r10, 2
    cmp rax, r10
    mov rax, 0
    setl al
    mov QWORD PTR [rbp-40], rax
    mov rax, QWORD PTR [rbp-40]
    cmp rax, 0
    je L_repeat_end_4
    sub rsp, 32
    lea rcx, .LC2[rip]
    call puts
    add rsp, 32
    mov rax, 1
    mov QWORD PTR [rbp-48], rax
    mov rax, QWORD PTR [rbp-32]
    mov r10, QWORD PTR [rbp-48]
    add rax, r10
    mov QWORD PTR [rbp-56], rax
    mov rax, QWORD PTR [rbp-56]
    mov QWORD PTR [rbp-32], rax
    jmp L_repeat_loop_3
L_repeat_end_4:
    mov rax, 2
    mov QWORD PTR [rbp-64], rax
    mov rax, QWORD PTR [rbp-64]
    mov r10, 1
    cmp rax, r10
    mov rax, 0
    sete al
    mov QWORD PTR [rbp-72], rax
    mov rax, QWORD PTR [rbp-72]
    cmp rax, 0
    jne L_switch_case_6
    mov rax, QWORD PTR [rbp-64]
    mov r10, 2
    cmp rax, r10
    mov rax, 0
    sete al
    mov QWORD PTR [rbp-80], rax
    mov rax, QWORD PTR [rbp-80]
    cmp rax, 0
    jne L_switch_case_7
    jmp L_switch_default_8
L_switch_case_6:
    sub rsp, 32
    lea rcx, .LC3[rip]
    call puts
    add rsp, 32
    jmp L_switch_end_5
L_switch_case_7:
    sub rsp, 32
    lea rcx, .LC4[rip]
    call puts
    add rsp, 32
    jmp L_switch_end_5
L_switch_default_8:
    sub rsp, 32
    lea rcx, .LC5[rip]
    call puts
    add rsp, 32
L_switch_end_5:
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 112
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "while:%lld\n"
.LC1: .asciz "do-once"
.LC2: .asciz "repeat"
.LC3: .asciz "one"
.LC4: .asciz "two"
.LC5: .asciz "other"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
