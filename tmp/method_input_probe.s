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
    sub rsp, 48
    # unresolved call target: util.log
    sub rsp, 32
    lea rcx, .LG1[rip]
    lea rdx, .L_INPUT_BUF[rip]
    call scanf
    add rsp, 32
    lea rax, .L_INPUT_BUF[rip]
    mov QWORD PTR [rbp-8], rax
    sub rsp, 32
    lea rcx, .LC1[rip]
    mov rdx, QWORD PTR [rbp-8]
    call printf
    add rsp, 32
    mov rax, 0
    jmp erelang_fn_main_end
erelang_fn_main_end:
    add rsp, 48
    pop rbp
    ret

.section .rdata,"dr"
.LC0: .asciz "probe"
.LC1: .asciz "name:%s\n"
.LG0: .asciz "%lld\\n"
.LG1: .asciz "%1023s"
.bss
.align 8
.L_INPUT_BUF: .space 1024
