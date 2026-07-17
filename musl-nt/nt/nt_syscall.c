#include "nt/ntpriv.h"

static nt_sc_t stat_path(nt_sc_t path, nt_sc_t out, int nofollow)
{
    return nt_sys_newfstatat(NT_AT_FDCWD, path, out,
                             nofollow ? NT_AT_SYMLINK_NOFOLLOW : 0);
}

static nt_sc_t vector_positional(nt_sc_t fd, nt_sc_t iov_arg, nt_sc_t count, nt_sc_t offset,
                              int write_operation)
{
    const struct nt_iovec *iov = (const void *)(uintptr_t)iov_arg;
    nt_sc_t total = 0, i;
    if (!iov && count) return -NT_EFAULT;
    if (count < 0 || count > 1024) return -NT_EINVAL;
    for (i = 0; i < count; ++i) {
        nt_sc_t r = write_operation
            ? nt_sys_pwrite(fd, (nt_sc_t)(uintptr_t)iov[i].iov_base,
                            (nt_sc_t)iov[i].iov_len, offset + total)
            : nt_sys_pread(fd, (nt_sc_t)(uintptr_t)iov[i].iov_base,
                           (nt_sc_t)iov[i].iov_len, offset + total);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
    }
    return total;
}

static nt_sc_t ppoll_backend(nt_sc_t fds, nt_sc_t count, nt_sc_t timeout_arg)
{
    const struct nt_timespec *timeout = (const void *)(uintptr_t)timeout_arg;
    nt_sc_t milliseconds = -1;
    if (timeout) {
        uint64_t value;
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000) return -NT_EINVAL;
        value = (uint64_t)timeout->tv_sec * 1000 +
                ((uint64_t)timeout->tv_nsec + 999999) / 1000000;
        milliseconds = value > 0x7fffffffU ? 0x7fffffffL : (nt_sc_t)value;
    }
    return nt_sys_poll(fds, count, milliseconds);
}

static NT_SYSV_ABI nt_sc_t nt_syscall_dispatch(nt_sc_t, nt_sc_t, nt_sc_t,
                                               nt_sc_t, nt_sc_t, nt_sc_t, nt_sc_t);

/* Choke point de TODAS as syscalls. Entrega sinais pendentes (SIGINT/Ctrl-C
 * etc.) na volta — os handlers Unix rodam aqui, na thread principal, em ponto
 * seguro (fora do meio de uma operação Win32). */
NT_SYSV_ABI nt_sc_t __nt_syscall(nt_sc_t n, nt_sc_t a1, nt_sc_t a2,
                                 nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                                 nt_sc_t a6)
{
    nt_sc_t r = nt_syscall_dispatch(n, a1, a2, a3, a4, a5, a6);
    nt_deliver_pending_signals();
    return r;
}

static NT_SYSV_ABI nt_sc_t nt_syscall_dispatch(nt_sc_t n, nt_sc_t a1, nt_sc_t a2,
                                 nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                                 nt_sc_t a6)
{
    switch (n) {
    case NT_SYS_read: return nt_sys_read(a1, a2, a3);
    case NT_SYS_write: return nt_sys_write(a1, a2, a3);
    case NT_SYS_open: return nt_sys_openat(NT_AT_FDCWD, a1, a2, a3);
    case NT_SYS_close: return nt_sys_close(a1);
    case NT_SYS_stat: return stat_path(a1, a2, 0);
    case NT_SYS_lstat: return stat_path(a1, a2, 1);
    case NT_SYS_fstat: return nt_sys_fstat(a1, a2);
    case NT_SYS_poll: return nt_sys_poll(a1, a2, a3);
    case NT_SYS_lseek: return nt_sys_lseek(a1, a2, a3);
    case NT_SYS_mmap: return nt_sys_mmap(a1, a2, a3, a4, a5, a6);
    case NT_SYS_mprotect: return nt_sys_mprotect(a1, a2, a3);
    case NT_SYS_munmap: return nt_sys_munmap(a1, a2);
    case NT_SYS_brk: return nt_sys_brk(a1);
    case NT_SYS_rt_sigaction: return nt_sys_rt_sigaction(a1, a2, a3, a4);
    case NT_SYS_rt_sigprocmask: return nt_sys_rt_sigprocmask(a1, a2, a3, a4);
    case NT_SYS_ioctl: return nt_sys_ioctl(a1, a2, a3);
    case NT_SYS_pread64: return nt_sys_pread(a1, a2, a3, a4);
    case NT_SYS_pwrite64: return nt_sys_pwrite(a1, a2, a3, a4);
    case NT_SYS_readv: return nt_sys_readv(a1, a2, a3);
    case NT_SYS_writev: return nt_sys_writev(a1, a2, a3);
    case NT_SYS_access: return nt_sys_faccessat(NT_AT_FDCWD, a1, a2, 0);
    case NT_SYS_pipe: return nt_sys_pipe2(a1, 0);
    case NT_SYS_sched_yield: SwitchToThread(); return 0;
    case NT_SYS_msync: return nt_sys_msync(a1, a2, a3);
    case NT_SYS_madvise: return nt_sys_madvise(a1, a2, a3);
    case NT_SYS_dup: return nt_sys_dup(a1);
    case NT_SYS_dup2: return nt_sys_dup2(a1, a2);
    case NT_SYS_nanosleep:
        return nt_sys_clock_nanosleep(NT_CLOCK_MONOTONIC, 0, a1, a2);
    case NT_SYS_getpid: return nt_sys_getpid();
    case NT_SYS_socket: return nt_sys_socket(a1, a2, a3);
    case NT_SYS_connect: return nt_sys_connect(a1, a2, a3);
    case NT_SYS_accept: return nt_sys_accept4(a1, a2, a3, 0);
    case NT_SYS_sendto: return nt_sys_sendto(a1, a2, a3, a4, a5, a6);
    case NT_SYS_recvfrom: return nt_sys_recvfrom(a1, a2, a3, a4, a5, a6);
    case NT_SYS_sendmsg: return nt_sys_sendmsg(a1, a2, a3);
    case NT_SYS_recvmsg: return nt_sys_recvmsg(a1, a2, a3);
    case NT_SYS_shutdown: return nt_sys_shutdown(a1, a2);
    case NT_SYS_bind: return nt_sys_bind(a1, a2, a3);
    case NT_SYS_listen: return nt_sys_listen(a1, a2);
    case NT_SYS_getsockname: return nt_sys_getsockname(a1, a2, a3);
    case NT_SYS_getpeername: return nt_sys_getpeername(a1, a2, a3);
    case NT_SYS_socketpair: return nt_sys_socketpair(a1, a2, a3, a4);
    case NT_SYS_setsockopt:
        return nt_sys_setsockopt(a1, a2, a3, a4, a5);
    case NT_SYS_getsockopt:
        return nt_sys_getsockopt(a1, a2, a3, a4, a5);
    case NT_SYS_fork:
    case NT_SYS_vfork: return nt_sys_fork();
    case NT_SYS_execve: return nt_sys_execve(a1, a2, a3);
    case NT_SYS_spawn: return nt_sys_spawn(a1, a2, a3);
    case NT_SYS_exit: return nt_sys_exit(a1);
    case NT_SYS_wait4: return nt_sys_wait4(a1, a2, a3, a4);
    case NT_SYS_kill: return nt_sys_kill(a1, a2);
    /* tkill(tid,sig)/tgkill(tgid,tid,sig): single-thread => sempre self ->
     * entrega local (é o que raise()/abort() da musl usam). */
    case NT_SYS_tkill: return nt_signal_post_local((int)a2);
    case NT_SYS_tgkill: return nt_signal_post_local((int)a3);
    case NT_SYS_fcntl: return nt_sys_fcntl(a1, a2, a3);
    case NT_SYS_fsync:
    case NT_SYS_fdatasync: return nt_sys_fsync(a1);
    case NT_SYS_truncate: return nt_sys_truncate(a1, a2);
    case NT_SYS_ftruncate: return nt_sys_ftruncate(a1, a2);
    case NT_SYS_getdents:
    case NT_SYS_getdents64: return nt_sys_getdents64(a1, a2, a3);
    case NT_SYS_getcwd: return nt_sys_getcwd(a1, a2);
    case NT_SYS_chdir: return nt_sys_chdir(a1);
    case NT_SYS_fchdir: return nt_sys_fchdir(a1);
    case NT_SYS_rename:
        return nt_sys_renameat2(NT_AT_FDCWD, a1, NT_AT_FDCWD, a2, 0);
    case NT_SYS_mkdir: return nt_sys_mkdirat(NT_AT_FDCWD, a1, a2);
    case NT_SYS_rmdir: return nt_sys_unlinkat(NT_AT_FDCWD, a1, NT_AT_REMOVEDIR);
    case NT_SYS_link:
        return nt_sys_linkat(NT_AT_FDCWD, a1, NT_AT_FDCWD, a2, 0);
    case NT_SYS_unlink: return nt_sys_unlinkat(NT_AT_FDCWD, a1, 0);
    case NT_SYS_symlink: return nt_sys_symlinkat(a1, NT_AT_FDCWD, a2);
    case NT_SYS_readlink: return nt_sys_readlinkat(NT_AT_FDCWD, a1, a2, a3);
    case NT_SYS_gettimeofday: return nt_sys_gettimeofday(a1, a2);
    case NT_SYS_getppid: return nt_sys_getppid();
    case NT_SYS_getpgrp: return nt_sys_getpid();
    case NT_SYS_setsid: return nt_sys_getpid();
    case NT_SYS_getpgid:
    case NT_SYS_getsid: return a1 ? a1 : nt_sys_getpid();
    case NT_SYS_statfs: return nt_sys_statfs(a1, a2);
    case NT_SYS_fstatfs: return nt_sys_fstatfs(a1, a2);
    case NT_SYS_futex: return nt_sys_futex(a1, a2, a3, a4, a5, a6);
    case NT_SYS_gettid: return (nt_sc_t)GetCurrentThreadId();
    case NT_SYS_time: {
        struct nt_timespec ts;
        nt_sc_t r = nt_sys_clock_gettime(NT_CLOCK_REALTIME, (nt_sc_t)(uintptr_t)&ts);
        if (r < 0) return r;
        if (a1) *(int64_t *)(uintptr_t)a1 = ts.tv_sec;
        return (nt_sc_t)ts.tv_sec;
    }
    case NT_SYS_set_tid_address: return nt_sys_set_tid_address(a1);
    case NT_SYS_clock_settime: return nt_sys_clock_settime(a1, a2);
    case NT_SYS_clock_gettime: return nt_sys_clock_gettime(a1, a2);
    case NT_SYS_clock_getres: return nt_sys_clock_getres(a1, a2);
    case NT_SYS_clock_nanosleep:
        return nt_sys_clock_nanosleep(a1, a2, a3, a4);
    case NT_SYS_exit_group: return nt_sys_exit(a1);
    case NT_SYS_openat: return nt_sys_openat(a1, a2, a3, a4);
    case NT_SYS_mkdirat: return nt_sys_mkdirat(a1, a2, a3);
    case NT_SYS_newfstatat: return nt_sys_newfstatat(a1, a2, a3, a4);
    case NT_SYS_unlinkat: return nt_sys_unlinkat(a1, a2, a3);
    case NT_SYS_renameat:
        return nt_sys_renameat2(a1, a2, a3, a4, 0);
    case NT_SYS_linkat: return nt_sys_linkat(a1, a2, a3, a4, a5);
    case NT_SYS_symlinkat: return nt_sys_symlinkat(a1, a2, a3);
    case NT_SYS_readlinkat: return nt_sys_readlinkat(a1, a2, a3, a4);
    case NT_SYS_faccessat: return nt_sys_faccessat(a1, a2, a3, 0);
    case NT_SYS_ppoll: return ppoll_backend(a1, a2, a3);
    case NT_SYS_utimensat:
        return nt_sys_stub(n, a1, a2, a3, a4, a5, a6);
    case NT_SYS_accept4: return nt_sys_accept4(a1, a2, a3, a4);
    case NT_SYS_recvmmsg: return nt_sys_recvmmsg(a1, a2, a3, a4, a5);
    case NT_SYS_dup3: return nt_sys_dup3(a1, a2, a3);
    case NT_SYS_pipe2: return nt_sys_pipe2(a1, a2);
    case NT_SYS_preadv: return vector_positional(a1, a2, a3, a4, 0);
    case NT_SYS_pwritev: return vector_positional(a1, a2, a3, a4, 1);
    case NT_SYS_syncfs: return nt_sys_fsync(a1);
    case NT_SYS_renameat2: return nt_sys_renameat2(a1, a2, a3, a4, a5);
    case NT_SYS_preadv2:
        if (a6) return -NT_ENOTSUP;
        return vector_positional(a1, a2, a3, a4, 0);
    case NT_SYS_pwritev2:
        if (a6 & ~0x20L) return -NT_ENOTSUP;
        return vector_positional(a1, a2, a3, a4, 1);
    case NT_SYS_statx: return nt_sys_statx(a1, a2, a3, a4, a5);
    case NT_SYS_faccessat2: return nt_sys_faccessat(a1, a2, a3, a4);
    case NT_SYS_sync: return nt_sys_sync();
    case NT_SYS_rt_sigpending:
        if (a1) nt_memset((void *)(uintptr_t)a1, 0, (size_t)a2);
        return 0;
    case NT_SYS_rt_sigsuspend: return -NT_EINTR;
    default: return nt_sys_stub(n, a1, a2, a3, a4, a5, a6);
    }
}
