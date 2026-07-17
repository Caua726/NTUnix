#include "nt/ntpriv.h"

static DWORD page_protection(nt_sc_t prot, int copy_on_write)
{
    if (prot == NT_PROT_NONE) return PAGE_NOACCESS;
    if (prot & NT_PROT_EXEC) {
        if (prot & NT_PROT_WRITE)
            return copy_on_write ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READWRITE;
        return (prot & NT_PROT_READ) ? PAGE_EXECUTE_READ : PAGE_EXECUTE;
    }
    if (prot & NT_PROT_WRITE)
        return copy_on_write ? PAGE_WRITECOPY : PAGE_READWRITE;
    return PAGE_READONLY;
}

nt_sc_t nt_sys_mmap(nt_sc_t addr_arg, nt_sc_t length_arg, nt_sc_t prot, nt_sc_t flags,
                 nt_sc_t fd, nt_sc_t off)
{
    void *requested = (void *)(uintptr_t)addr_arg;
    size_t length;
    DWORD protect;
    void *mapped;
    SYSTEM_INFO info;
    if (length_arg <= 0 || off < 0) return -NT_EINVAL;
    length = (size_t)length_arg;
    if ((flags & (NT_MAP_SHARED | NT_MAP_PRIVATE)) == 0) return -NT_EINVAL;
    protect = page_protection(prot, !(flags & NT_MAP_ANONYMOUS) &&
                                    (flags & NT_MAP_PRIVATE));
    if (flags & NT_MAP_ANONYMOUS) {
        mapped = VirtualAlloc(requested, length, MEM_RESERVE | MEM_COMMIT,
                              protect);
        if (!mapped) return nt_last_error();
        if ((flags & (NT_MAP_FIXED | NT_MAP_FIXED_NOREPLACE)) &&
            mapped != requested) {
            VirtualFree(mapped, 0, MEM_RELEASE);
            return -NT_EEXIST;
        }
        return (nt_sc_t)(uintptr_t)mapped;
    }
    {
        struct nt_fd *slot = nt_fd_get((int)fd);
        HANDLE mapping;
        DWORD map_access;
        ULARGE_INTEGER maximum, offset;
        if (!slot) return -NT_EBADF;
        if (slot->kind != NT_FD_FILE) return -NT_ENODEV;
        GetSystemInfo(&info);
        if ((uint64_t)off % info.dwAllocationGranularity) return -NT_EINVAL;
        maximum.QuadPart = (uint64_t)off + length;
        mapping = CreateFileMappingW(slot->handle, 0, protect,
                                     maximum.HighPart, maximum.LowPart, 0);
        if (!mapping) return nt_last_error();
        if (flags & NT_MAP_PRIVATE)
            map_access = (prot & NT_PROT_EXEC) ? FILE_MAP_COPY | FILE_MAP_EXECUTE
                                               : FILE_MAP_COPY;
        else {
            map_access = (prot & NT_PROT_WRITE) ? FILE_MAP_WRITE : FILE_MAP_READ;
            if (prot & NT_PROT_EXEC) map_access |= FILE_MAP_EXECUTE;
        }
        offset.QuadPart = (uint64_t)off;
        mapped = MapViewOfFileEx(mapping, map_access, offset.HighPart,
                                 offset.LowPart, length,
                                 (flags & (NT_MAP_FIXED | NT_MAP_FIXED_NOREPLACE))
                                     ? requested : 0);
        CloseHandle(mapping);
        if (!mapped) return nt_last_error();
        if ((flags & (NT_MAP_FIXED | NT_MAP_FIXED_NOREPLACE)) &&
            mapped != requested) {
            UnmapViewOfFile(mapped);
            return -NT_EEXIST;
        }
        return (nt_sc_t)(uintptr_t)mapped;
    }
}

nt_sc_t nt_sys_munmap(nt_sc_t addr, nt_sc_t length)
{
    MEMORY_BASIC_INFORMATION info;
    void *p = (void *)(uintptr_t)addr;
    if (!p || length <= 0) return -NT_EINVAL;
    if (!VirtualQuery(p, &info, sizeof info)) return nt_last_error();
    if (info.Type == MEM_MAPPED || info.Type == MEM_IMAGE) {
        if (!UnmapViewOfFile(info.AllocationBase)) return nt_last_error();
    } else {
        if (p == info.AllocationBase) {
            if (!VirtualFree(p, 0, MEM_RELEASE)) return nt_last_error();
        } else if (!VirtualFree(p, (size_t)length, MEM_DECOMMIT)) {
            return nt_last_error();
        }
    }
    return 0;
}

nt_sc_t nt_sys_mprotect(nt_sc_t addr, nt_sc_t length, nt_sc_t prot)
{
    DWORD old;
    if (!addr || length <= 0) return -NT_EINVAL;
    if (!VirtualProtect((void *)(uintptr_t)addr, (size_t)length,
                        page_protection(prot, 0), &old))
        return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_msync(nt_sc_t addr, nt_sc_t length, nt_sc_t flags)
{
    if (!addr || length <= 0) return -NT_EINVAL;
    if (flags & ~(NT_MS_ASYNC | NT_MS_SYNC)) return -NT_EINVAL;
    if (!FlushViewOfFile((void *)(uintptr_t)addr, (size_t)length)) {
        DWORD e = GetLastError();
        /* Anonymous mappings have nothing to flush. */
        if (e != ERROR_INVALID_PARAMETER) return nt_error(e);
    }
    return 0;
}

nt_sc_t nt_sys_madvise(nt_sc_t addr, nt_sc_t length, nt_sc_t advice)
{
    typedef DWORD (WINAPI *discard_fn)(PVOID, SIZE_T);
    static discard_fn discard;
    if (!addr || length <= 0) return -NT_EINVAL;
    if (advice == 4) { /* MADV_DONTNEED */
        if (!discard) {
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (kernel)
                discard = (discard_fn)(void *)GetProcAddress(kernel,
                                                              "DiscardVirtualMemory");
        }
        if (discard) {
            DWORD e = discard((void *)(uintptr_t)addr, (size_t)length);
            if (e != ERROR_SUCCESS) return nt_error(e);
        }
    }
    return 0;
}

#define NT_BRK_RESERVE (256ULL * 1024ULL * 1024ULL)
static INIT_ONCE brk_once = INIT_ONCE_STATIC_INIT;
static SRWLOCK brk_lock = SRWLOCK_INIT;
static unsigned char *brk_base;
static unsigned char *brk_current;
static unsigned char *brk_committed;

static BOOL CALLBACK initialize_brk(PINIT_ONCE once, PVOID parameter,
                                    PVOID *context)
{
    (void)once; (void)parameter; (void)context;
    brk_base = VirtualAlloc(0, NT_BRK_RESERVE, MEM_RESERVE, PAGE_READWRITE);
    brk_current = brk_committed = brk_base;
    return TRUE;
}

nt_sc_t nt_sys_brk(nt_sc_t addr_arg)
{
    unsigned char *wanted = (unsigned char *)(uintptr_t)addr_arg;
    unsigned char *rounded;
    SYSTEM_INFO info;
    InitOnceExecuteOnce(&brk_once, initialize_brk, 0, 0);
    if (!brk_base) return 0;
    AcquireSRWLockExclusive(&brk_lock);
    if (!wanted) {
        nt_sc_t result = (nt_sc_t)(uintptr_t)brk_current;
        ReleaseSRWLockExclusive(&brk_lock);
        return result;
    }
    if (wanted < brk_base || wanted > brk_base + NT_BRK_RESERVE) {
        nt_sc_t result = (nt_sc_t)(uintptr_t)brk_current;
        ReleaseSRWLockExclusive(&brk_lock);
        return result;
    }
    GetSystemInfo(&info);
    rounded = (unsigned char *)(((uintptr_t)wanted + info.dwPageSize - 1) &
                                ~(uintptr_t)(info.dwPageSize - 1));
    if (rounded > brk_committed) {
        if (!VirtualAlloc(brk_committed, (size_t)(rounded - brk_committed),
                          MEM_COMMIT, PAGE_READWRITE)) {
            nt_sc_t result = (nt_sc_t)(uintptr_t)brk_current;
            ReleaseSRWLockExclusive(&brk_lock);
            return result;
        }
        brk_committed = rounded;
    } else if (rounded < brk_committed) {
        VirtualFree(rounded, (size_t)(brk_committed - rounded), MEM_DECOMMIT);
        brk_committed = rounded;
    }
    brk_current = wanted;
    ReleaseSRWLockExclusive(&brk_lock);
    return (nt_sc_t)(uintptr_t)wanted;
}
