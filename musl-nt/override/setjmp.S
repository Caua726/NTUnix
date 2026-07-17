    .text
    .globl setjmp
    .globl _setjmp
    .globl __setjmp
    .def setjmp; .scl 2; .type 32; .endef
    .def _setjmp; .scl 2; .type 32; .endef
    .def __setjmp; .scl 2; .type 32; .endef
setjmp:
_setjmp:
__setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret

    .globl longjmp
    .globl _longjmp
    .def longjmp; .scl 2; .type 32; .endef
    .def _longjmp; .scl 2; .type 32; .endef
longjmp:
_longjmp:
    movq %rdi, %r8
    movl %esi, %eax
    testl %eax, %eax
    jne 1f
    incl %eax
1:
    movq 0(%r8), %rbx
    movq 8(%r8), %rbp
    movq 16(%r8), %r12
    movq 24(%r8), %r13
    movq 32(%r8), %r14
    movq 40(%r8), %r15
    movq 48(%r8), %rsp
    jmp *56(%r8)
