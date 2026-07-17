/*
 * Windows/NT replacement for musl's architecture-specific syscall_cp.s.
 * NTUnix v0 has cooperative cancellation only: the wrapper observes the
 * musl cancellation word before entering the backend.  The signal backend
 * does not rewrite an interrupted thread's instruction pointer, so the three
 * marker symbols exist solely to satisfy pthread_cancel.c.
 */
extern long __nt_syscall(long, long, long, long, long, long, long);

const char __cp_begin[1];
const char __cp_end[1];
const char __cp_cancel[1];

long __syscall_cp_asm(volatile void *cancel, long number,
                      long a1, long a2, long a3,
                      long a4, long a5, long a6)
{
    if (*(volatile int *)cancel)
        return -4; /* -EINTR: __syscall_cp_c performs the cancellation. */
    return __nt_syscall(number, a1, a2, a3, a4, a5, a6);
}
