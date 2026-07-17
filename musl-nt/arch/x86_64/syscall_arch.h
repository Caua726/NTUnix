#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __nt_syscall(long, long, long, long, long, long, long);

static __inline long __syscall0(long n)
{
    return __nt_syscall(n, 0, 0, 0, 0, 0, 0);
}
static __inline long __syscall1(long n, long a1)
{
    return __nt_syscall(n, a1, 0, 0, 0, 0, 0);
}
static __inline long __syscall2(long n, long a1, long a2)
{
    return __nt_syscall(n, a1, a2, 0, 0, 0, 0);
}
static __inline long __syscall3(long n, long a1, long a2, long a3)
{
    return __nt_syscall(n, a1, a2, a3, 0, 0, 0);
}
static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
    return __nt_syscall(n, a1, a2, a3, a4, 0, 0);
}
static __inline long __syscall5(long n, long a1, long a2, long a3, long a4,
                                long a5)
{
    return __nt_syscall(n, a1, a2, a3, a4, a5, 0);
}
static __inline long __syscall6(long n, long a1, long a2, long a3, long a4,
                                long a5, long a6)
{
    return __nt_syscall(n, a1, a2, a3, a4, a5, a6);
}

#define IPC_64 0
