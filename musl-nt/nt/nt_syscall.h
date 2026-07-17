#ifndef MUSL_NT_SYSCALL_H
#define MUSL_NT_SYSCALL_H

#ifdef __cplusplus
extern "C" {
#endif

NT_SYSV_ABI nt_sc_t __nt_syscall(nt_sc_t number, nt_sc_t a1, nt_sc_t a2,
                                 nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                                 nt_sc_t a6);

#ifdef __cplusplus
}
#endif
#endif
