#include "nt/ntpriv.h"

#define NT_STATUS_NO_MORE_FILES ((NTSTATUS)0x80000006L)
#define NT_RENAME_NOREPLACE 1U

typedef NTSTATUS (NTAPI *nt_query_directory_fn)(
    HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK, PVOID, ULONG,
    FILE_INFORMATION_CLASS, BOOLEAN, PUNICODE_STRING, BOOLEAN);

static nt_query_directory_fn query_directory;
static INIT_ONCE query_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK initialize_query(PINIT_ONCE once, PVOID parameter,
                                      PVOID *context)
{
    HMODULE ntdll;
    (void)once; (void)parameter; (void)context;
    ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
        query_directory = (nt_query_directory_fn)(void *)
            GetProcAddress(ntdll, "NtQueryDirectoryFile");
    return TRUE;
}

static size_t align8(size_t value)
{
    return (value + 7U) & ~(size_t)7U;
}

/* DEBUG TEMPORARIO (bug do `ls`): loga "label=0xHEX" em X:\NTUnix\ls-debug.log.
 * Remover depois de diagnosticar. */
static void ls_dbg(const char *label, unsigned long long val)
{
    char buf[80];
    int p = 0;
    while (*label && p < 40) buf[p++] = *label++;
    buf[p++] = '='; buf[p++] = '0'; buf[p++] = 'x';
    for (int i = 60; i >= 0; i -= 4)
        buf[p++] = "0123456789abcdef"[(val >> i) & 0xf];
    buf[p++] = '\n';
    HANDLE h = CreateFileW(L"X:\\NTUnix\\ls-debug.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, buf, (DWORD)p, &w, 0);
        CloseHandle(h);
    }
}

/* DEBUG: loga "nm[LEN]=<nome>" p/ ver se os nomes das entradas saem certos. */
static void ls_dbg_name(const char *name, size_t len)
{
    char buf[96];
    int p = 0;
    buf[p++] = 'n'; buf[p++] = 'm'; buf[p++] = '[';
    buf[p++] = "0123456789"[(len / 10) % 10];
    buf[p++] = "0123456789"[len % 10];
    buf[p++] = ']'; buf[p++] = '=';
    for (size_t i = 0; i < len && p < 90; i++)
        buf[p++] = (name[i] >= 0x20) ? name[i] : '?';
    buf[p++] = '\n';
    HANDLE h = CreateFileW(L"X:\\NTUnix\\ls-debug.log", FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_ALWAYS, 0, 0);
    if (h != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(h, buf, (DWORD)p, &w, 0); CloseHandle(h); }
}

nt_sc_t nt_sys_getdents64(nt_sc_t fd, nt_sc_t buf_arg, nt_sc_t count_arg)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    unsigned char *out = (void *)(uintptr_t)buf_arg;
    size_t cap, used = 0;
    unsigned char query_buffer[2048];
    IO_STATUS_BLOCK iosb;
    if (!slot) { ls_dbg("gd_nofd", (unsigned long long)fd); return -NT_EBADF; }
    ls_dbg("gd_fd", (unsigned long long)fd);
    ls_dbg("gd_kind", (unsigned long long)slot->kind);
    ls_dbg("gd_count", (unsigned long long)count_arg);
    if (slot->kind != NT_FD_DIR) return -NT_ENOTDIR;
    if (!out && count_arg) return -NT_EFAULT;
    if (count_arg <= 0) return -NT_EINVAL;
    cap = (size_t)count_arg;
    InitOnceExecuteOnce(&query_once, initialize_query, 0, 0);
    if (!query_directory) return -NT_ENOSYS;
    AcquireSRWLockExclusive(&slot->io_lock);
    if (slot->dir_eof) {
        ReleaseSRWLockExclusive(&slot->io_lock);
        ls_dbg("gd_eof_early", 0);
        return 0;
    }
    for (;;) {
        FILE_ID_BOTH_DIR_INFORMATION *info;
        char name[1024];
        size_t name_len;
        size_t record_len;
        struct nt_linux_dirent64 *dent;
        NTSTATUS status;
        nt_memset(&iosb, 0, sizeof iosb);
        status = query_directory(slot->handle, 0, 0, 0, &iosb,
                                 query_buffer, sizeof query_buffer,
                                 FileIdBothDirectoryInformation, TRUE, 0,
                                 slot->dir_cookie == 0);
        if (slot->dir_cookie < 3)
            ls_dbg("gd_status", (unsigned long long)(unsigned long)status);
        if (status == NT_STATUS_NO_MORE_FILES) {
            slot->dir_eof = 1;
            break;
        }
        if (status < 0) {
            nt_sc_t r = nt_error_from_status(status);
            ReleaseSRWLockExclusive(&slot->io_lock);
            ls_dbg("gd_err_status", (unsigned long long)(unsigned long)status);
            ls_dbg("gd_err_errno", (unsigned long long)(unsigned long long)(-r));
            return used ? (nt_sc_t)used : r;
        }
        info = (void *)query_buffer;
        if (nt_wide_to_utf8(info->FileName, info->FileNameLength / sizeof(WCHAR),
                            name, sizeof name, &name_len) < 0) {
            ReleaseSRWLockExclusive(&slot->io_lock);
            return used ? (nt_sc_t)used : -NT_ENAMETOOLONG;
        }
        if (slot->dir_cookie < 8) {   /* DEBUG: nomes das primeiras entradas */
            ls_dbg("nm_rawlen", (unsigned long long)info->FileNameLength);
            ls_dbg_name(name, name_len);
        }
        record_len = align8(offsetof(struct nt_linux_dirent64, d_name) +
                            name_len + 1);
        if (record_len > cap - used) {
            /* A musl DIR buffer is 2048 bytes, so every legal NTFS name fits.
             * If a smaller direct syscall buffer is supplied, report EINVAL. */
            if (!used) {
                ReleaseSRWLockExclusive(&slot->io_lock);
                return -NT_EINVAL;
            }
            break;
        }
        dent = (void *)(out + used);
        nt_memset(dent, 0, record_len);
        dent->d_ino = (uint64_t)info->FileId.QuadPart;
        dent->d_off = (int64_t)++slot->dir_cookie;
        dent->d_reclen = (uint16_t)record_len;
        dent->d_type = nt_dtype_from_attributes(info->FileAttributes, 0);
        nt_memcpy(dent->d_name, name, name_len + 1);
        used += record_len;
        if (cap - used < offsetof(struct nt_linux_dirent64, d_name) + 2)
            break;
    }
    ReleaseSRWLockExclusive(&slot->io_lock);
    ls_dbg("gd_used", (unsigned long long)used);
    return (nt_sc_t)used;
}

nt_sc_t nt_sys_mkdirat(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t mode)
{
    WCHAR path[NT_PATH_CAP];
    nt_sc_t r;
    (void)mode;
    r = nt_path_at((int)dirfd, (const char *)(uintptr_t)path_arg,
                   path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    if (!CreateDirectoryW(path, 0)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_unlinkat(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t flags)
{
    WCHAR path[NT_PATH_CAP];
    DWORD attrs;
    nt_sc_t r;
    if (flags & ~NT_AT_REMOVEDIR) return -NT_EINVAL;
    r = nt_path_at((int)dirfd, (const char *)(uintptr_t)path_arg,
                   path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return nt_last_error();
    if (flags & NT_AT_REMOVEDIR) {
        if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -NT_ENOTDIR;
        if (!RemoveDirectoryW(path)) return nt_last_error();
    } else {
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) return -NT_EISDIR;
        if (!DeleteFileW(path)) return nt_last_error();
    }
    return 0;
}

nt_sc_t nt_sys_renameat2(nt_sc_t olddirfd, nt_sc_t oldpath_arg, nt_sc_t newdirfd,
                      nt_sc_t newpath_arg, nt_sc_t flags)
{
    WCHAR oldpath[NT_PATH_CAP], newpath[NT_PATH_CAP];
    DWORD move_flags = 0;
    nt_sc_t r;
    if (flags & ~NT_RENAME_NOREPLACE) return -NT_ENOSYS;
    r = nt_path_at((int)olddirfd, (const char *)(uintptr_t)oldpath_arg,
                   oldpath, NT_ARRAY_LEN(oldpath));
    if (r < 0) return r;
    r = nt_path_at((int)newdirfd, (const char *)(uintptr_t)newpath_arg,
                   newpath, NT_ARRAY_LEN(newpath));
    if (r < 0) return r;
    if (!(flags & NT_RENAME_NOREPLACE)) move_flags |= MOVEFILE_REPLACE_EXISTING;
    if (!MoveFileExW(oldpath, newpath, move_flags)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_linkat(nt_sc_t olddirfd, nt_sc_t oldpath_arg, nt_sc_t newdirfd,
                   nt_sc_t newpath_arg, nt_sc_t flags)
{
    WCHAR oldpath[NT_PATH_CAP], newpath[NT_PATH_CAP];
    nt_sc_t r;
    if (flags & ~NT_AT_SYMLINK_FOLLOW) return -NT_EINVAL;
    r = nt_path_at((int)olddirfd, (const char *)(uintptr_t)oldpath_arg,
                   oldpath, NT_ARRAY_LEN(oldpath));
    if (r < 0) return r;
    r = nt_path_at((int)newdirfd, (const char *)(uintptr_t)newpath_arg,
                   newpath, NT_ARRAY_LEN(newpath));
    if (r < 0) return r;
    if (!CreateHardLinkW(newpath, oldpath, 0)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_symlinkat(nt_sc_t target_arg, nt_sc_t newdirfd, nt_sc_t newpath_arg)
{
    typedef BOOLEAN (WINAPI *create_symlink_fn)(LPCWSTR, LPCWSTR, DWORD);
    static create_symlink_fn create_symlink;
    const char *target_utf8 = (const char *)(uintptr_t)target_arg;
    WCHAR target[NT_PATH_CAP], newpath[NT_PATH_CAP];
    DWORD flags = SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE;
    DWORD attrs;
    nt_sc_t r;
    if (!target_utf8 || !newpath_arg) return -NT_EFAULT;
    if (target_utf8[0] == '/' || target_utf8[0] == '\\' ||
        (target_utf8[0] && target_utf8[1] == ':'))
        r = nt_path_at(NT_AT_FDCWD, target_utf8, target, NT_ARRAY_LEN(target));
    else
        r = nt_utf8_to_wide(target_utf8, target, NT_ARRAY_LEN(target));
    if (r < 0) return r;
    r = nt_path_at((int)newdirfd, (const char *)(uintptr_t)newpath_arg,
                   newpath, NT_ARRAY_LEN(newpath));
    if (r < 0) return r;
    attrs = GetFileAttributesW(target);
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
        flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
    if (!create_symlink) {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        if (kernel)
            create_symlink = (create_symlink_fn)(void *)
                GetProcAddress(kernel, "CreateSymbolicLinkW");
    }
    if (!create_symlink) return -NT_ENOSYS;
    if (!create_symlink(newpath, target, flags)) return nt_last_error();
    return 0;
}
