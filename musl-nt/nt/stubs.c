#include "nt/ntpriv.h"

static SRWLOCK umask_lock = SRWLOCK_INIT;
static unsigned int process_umask = 0022;

static void copy_text(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; ++i; }
    dst[i] = 0;
}

static nt_sc_t stub_uname(struct nt_utsname *name)
{
    WCHAR computer[256];
    DWORD n = NT_ARRAY_LEN(computer);
    if (!name) return -NT_EFAULT;
    nt_memset(name, 0, sizeof *name);
    copy_text(name->sysname, sizeof name->sysname, "NTUnix");
    copy_text(name->release, sizeof name->release, "0.1.0-nt");
    copy_text(name->version, sizeof name->version, "musl-nt");
    copy_text(name->machine, sizeof name->machine, "x86_64");
    if (GetComputerNameW(computer, &n))
        nt_wide_to_utf8(computer, n, name->nodename, sizeof name->nodename, 0);
    else
        copy_text(name->nodename, sizeof name->nodename, "localhost");
    return 0;
}

static nt_sc_t stub_getrandom(void *buffer, size_t length, unsigned int flags)
{
    typedef BOOLEAN (WINAPI *random_fn)(PVOID, ULONG);
    static random_fn random_bytes;
    size_t done = 0;
    (void)flags;
    if (!buffer && length) return -NT_EFAULT;
    if (!random_bytes) {
        HMODULE advapi = LoadLibraryW(L"advapi32.dll");
        if (advapi)
            random_bytes = (random_fn)(void *)GetProcAddress(advapi,
                                                          "SystemFunction036");
    }
    if (!random_bytes) return -NT_ENOSYS;
    while (done < length) {
        ULONG chunk = length - done > 0xffffffffU
                        ? 0xffffffffU : (ULONG)(length - done);
        if (!random_bytes((unsigned char *)buffer + done, chunk)) return -NT_EIO;
        done += chunk;
    }
    return (nt_sc_t)done;
}

static nt_sc_t stub_getrlimit(struct nt_rlimit *limit)
{
    if (!limit) return -NT_EFAULT;
    limit->rlim_cur = UINT64_MAX;
    limit->rlim_max = UINT64_MAX;
    return 0;
}

static nt_sc_t stub_sched_affinity(void *mask, size_t size)
{
    DWORD_PTR process_mask, system_mask;
    if (!mask || !size) return -NT_EINVAL;
    nt_memset(mask, 0, size);
    if (!GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask))
        return nt_last_error();
    nt_memcpy(mask, &process_mask,
              size < sizeof process_mask ? size : sizeof process_mask);
    return (nt_sc_t)(size < sizeof process_mask ? size : sizeof process_mask);
}

static nt_sc_t stub_utimensat(nt_sc_t dirfd, const char *path,
                           const struct nt_timespec *times, nt_sc_t flags)
{
    WCHAR wide[NT_PATH_CAP];
    HANDLE h;
    FILETIME access, write;
    FILETIME *access_ptr = 0, *write_ptr = 0;
    struct nt_timespec now;
    nt_sc_t r;
    if (!path) return -NT_EFAULT;
    if (flags & ~NT_AT_SYMLINK_NOFOLLOW) return -NT_EINVAL;
    r = nt_path_at((int)dirfd, path, wide, NT_ARRAY_LEN(wide));
    if (r < 0) return r;
    h = CreateFileW(wide, FILE_WRITE_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                    ((flags & NT_AT_SYMLINK_NOFOLLOW)
                         ? FILE_FLAG_OPEN_REPARSE_POINT : 0), 0);
    if (h == INVALID_HANDLE_VALUE) return nt_last_error();
    if (!times) {
        nt_sys_clock_gettime(NT_CLOCK_REALTIME, (nt_sc_t)(uintptr_t)&now);
        access = write = nt_timespec_to_filetime(&now);
        access_ptr = &access; write_ptr = &write;
    } else {
        if (times[0].tv_nsec == NT_UTIME_NOW) {
            nt_sys_clock_gettime(NT_CLOCK_REALTIME, (nt_sc_t)(uintptr_t)&now);
            access = nt_timespec_to_filetime(&now); access_ptr = &access;
        } else if (times[0].tv_nsec != NT_UTIME_OMIT) {
            access = nt_timespec_to_filetime(&times[0]); access_ptr = &access;
        }
        if (times[1].tv_nsec == NT_UTIME_NOW) {
            nt_sys_clock_gettime(NT_CLOCK_REALTIME, (nt_sc_t)(uintptr_t)&now);
            write = nt_timespec_to_filetime(&now); write_ptr = &write;
        } else if (times[1].tv_nsec != NT_UTIME_OMIT) {
            write = nt_timespec_to_filetime(&times[1]); write_ptr = &write;
        }
    }
    if (!SetFileTime(h, 0, access_ptr, write_ptr)) r = nt_last_error();
    else r = 0;
    CloseHandle(h);
    return r;
}

nt_sc_t nt_sys_futex(nt_sc_t address_arg, nt_sc_t operation, nt_sc_t value,
                  nt_sc_t timeout_arg, nt_sc_t address2, nt_sc_t value3)
{
    volatile LONG *address = (volatile LONG *)(uintptr_t)address_arg;
    const struct nt_timespec *timeout = (const void *)(uintptr_t)timeout_arg;
    DWORD milliseconds = INFINITE;
    int command = (int)operation & 0x7f;
    (void)address2; (void)value3;
    if (!address) return -NT_EFAULT;
    if (command == 0) { /* FUTEX_WAIT */
        LONG expected = (LONG)value;
        if (InterlockedCompareExchange(address, 0, 0) != expected)
            return -NT_EAGAIN;
        if (timeout) {
            uint64_t ms;
            if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                timeout->tv_nsec >= 1000000000) return -NT_EINVAL;
            ms = (uint64_t)timeout->tv_sec * 1000 +
                 ((uint64_t)timeout->tv_nsec + 999999) / 1000000;
            milliseconds = ms >= INFINITE ? INFINITE - 1 : (DWORD)ms;
        }
        if (!WaitOnAddress((volatile VOID *)address, &expected,
                           sizeof expected, milliseconds)) {
            DWORD e = GetLastError();
            return e == ERROR_TIMEOUT ? -NT_ETIMEDOUT : nt_error(e);
        }
        return 0;
    }
    if (command == 1) { /* FUTEX_WAKE */
        if (value <= 0) return 0;
        if (value == 1) WakeByAddressSingle((PVOID)address);
        else WakeByAddressAll((PVOID)address);
        return value;
    }
    return -NT_ENOSYS;
}

nt_sc_t nt_sys_stub(nt_sc_t number, nt_sc_t a1, nt_sc_t a2, nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                 nt_sc_t a6)
{
    (void)a4; (void)a5; (void)a6;
    switch (number) {
    case NT_SYS_getuid:
    case NT_SYS_getgid:
    case NT_SYS_geteuid:
    case NT_SYS_getegid:
        return 0;
    case NT_SYS_getgroups:
        if (a1 < 0) return -NT_EINVAL;
        return 0;
    case NT_SYS_setuid:
    case NT_SYS_setgid:
        return a1 == 0 ? 0 : -NT_EPERM;
    case NT_SYS_setreuid:
    case NT_SYS_setregid:
        return ((a1 == 0 || a1 == -1) && (a2 == 0 || a2 == -1))
                 ? 0 : -NT_EPERM;
    case NT_SYS_setresuid:
    case NT_SYS_setresgid:
        return ((a1 == 0 || a1 == -1) && (a2 == 0 || a2 == -1) &&
                (a3 == 0 || a3 == -1)) ? 0 : -NT_EPERM;
    case NT_SYS_getresuid:
    case NT_SYS_getresgid:
        if (a1) *(unsigned *)(uintptr_t)a1 = 0;
        if (a2) *(unsigned *)(uintptr_t)a2 = 0;
        if (a3) *(unsigned *)(uintptr_t)a3 = 0;
        return 0;
    case NT_SYS_umask: {
        unsigned old;
        AcquireSRWLockExclusive(&umask_lock);
        old = process_umask;
        process_umask = (unsigned)a1 & 0777;
        ReleaseSRWLockExclusive(&umask_lock);
        return old;
    }
    case NT_SYS_chmod:
    case NT_SYS_fchmod:
    case NT_SYS_chown:
    case NT_SYS_fchown:
    case NT_SYS_lchown:
    case NT_SYS_fchownat:
    case NT_SYS_fchmodat:
    case NT_SYS_fchmodat2:
        return 0; /* v0 presents synthetic modes and uid/gid 0. */
    case NT_SYS_uname:
        return stub_uname((void *)(uintptr_t)a1);
    case NT_SYS_getrlimit:
        return stub_getrlimit((void *)(uintptr_t)a2);
    case NT_SYS_prlimit64:
        if (a4) {
            struct nt_rlimit *limit = (void *)(uintptr_t)a4;
            limit->rlim_cur = limit->rlim_max = UINT64_MAX;
        }
        return a3 ? 0 : 0;
    case NT_SYS_sched_getaffinity:
        return stub_sched_affinity((void *)(uintptr_t)a3, (size_t)a2);
    case NT_SYS_sched_yield:
        SwitchToThread(); return 0;
    case NT_SYS_set_robust_list:
        return 0;
    case NT_SYS_getrandom:
        return stub_getrandom((void *)(uintptr_t)a1, (size_t)a2, (unsigned)a3);
    case NT_SYS_utimensat:
        return stub_utimensat(a1, (const char *)(uintptr_t)a2,
                              (const void *)(uintptr_t)a3, a4);
    case NT_SYS_alarm:
        return 0;
    case NT_SYS_getpriority:
        return 20;
    case NT_SYS_setpriority:
        return 0;
    case NT_SYS_fork:
    case NT_SYS_vfork:
        return -NT_ENOSYS;
    default:
        return -NT_ENOSYS;
    }
}
