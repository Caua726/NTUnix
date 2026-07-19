#include "nt/ntpriv.h"

static nt_sc_t open_for_metadata(int dirfd, const char *path, int flags,
                              HANDLE *result)
{
    WCHAR wide[NT_PATH_CAP];
    DWORD attrs = FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE h;
    nt_sc_t r = nt_path_at(dirfd, path, wide, NT_ARRAY_LEN(wide));
    if (r < 0) return r;
    if (flags & NT_AT_SYMLINK_NOFOLLOW) attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
    h = CreateFileW(wide, FILE_READ_ATTRIBUTES | SYNCHRONIZE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, OPEN_EXISTING, attrs, 0);
    if (h == INVALID_HANDLE_VALUE) return nt_last_error();
    *result = h;
    return 0;
}

nt_sc_t nt_sys_statx(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t flags, nt_sc_t mask,
                  nt_sc_t out_arg)
{
    const char *path = (const char *)(uintptr_t)path_arg;
    struct nt_statx *out = (void *)(uintptr_t)out_arg;
    HANDLE h;
    int close_handle = 0;
    nt_sc_t r;
    (void)mask;
    if (!path || !out) return -NT_EFAULT;
    if (flags & ~(NT_AT_SYMLINK_NOFOLLOW | NT_AT_EMPTY_PATH | 0x6000))
        return -NT_EINVAL;
    if (!*path && (flags & NT_AT_EMPTY_PATH)) {
        h = nt_fd_handle((int)dirfd);
        if (h == INVALID_HANDLE_VALUE) return -NT_EBADF;
    } else {
        r = open_for_metadata((int)dirfd, path, (int)flags, &h);
        if (r < 0) return r;
        close_handle = 1;
    }
    r = nt_stat_from_handle(h, out);
    if (close_handle) CloseHandle(h);
    return r;
}

nt_sc_t nt_sys_fstat(nt_sc_t fd, nt_sc_t out_arg)
{
    struct nt_stat *out = (void *)(uintptr_t)out_arg;
    struct nt_statx statx;
    HANDLE h;
    nt_sc_t r;
    if (!out) return -NT_EFAULT;
    h = nt_fd_handle((int)fd);
    if (h == INVALID_HANDLE_VALUE) return -NT_EBADF;
    r = nt_stat_from_handle(h, &statx);
    if (r < 0) return r;
    nt_statx_to_stat(&statx, out);
    return 0;
}

nt_sc_t nt_sys_newfstatat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t out_arg, nt_sc_t flags)
{
    struct nt_stat *out = (void *)(uintptr_t)out_arg;
    struct nt_statx statx;
    nt_sc_t r;
    if (!out) return -NT_EFAULT;
    r = nt_sys_statx(dirfd, path, flags, NT_STATX_BASIC_STATS,
                     (nt_sc_t)(uintptr_t)&statx);
    if (r < 0) return r;
    nt_statx_to_stat(&statx, out);
    return 0;
}

static int wide_suffix_ci(const WCHAR *path, const WCHAR *suffix)
{
    size_t np = nt_wcslen(path), ns = nt_wcslen(suffix), i;
    if (ns > np) return 0;
    path += np - ns;
    for (i = 0; i < ns; ++i) {
        WCHAR a = path[i], b = suffix[i];
        if (a >= L'A' && a <= L'Z') a += L'a' - L'A';
        if (b >= L'A' && b <= L'Z') b += L'a' - L'A';
        if (a != b) return 0;
    }
    return 1;
}

nt_sc_t nt_sys_faccessat(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t mode, nt_sc_t flags)
{
    const char *path = (const char *)(uintptr_t)path_arg;
    WCHAR wide[NT_PATH_CAP];
    DWORD attrs;
    nt_sc_t r;
    if (!path) return -NT_EFAULT;
    if (mode & ~7) return -NT_EINVAL;
    if (flags & ~(NT_AT_SYMLINK_NOFOLLOW | 0x200)) return -NT_EINVAL;
    r = nt_path_at((int)dirfd, path, wide, NT_ARRAY_LEN(wide));
    if (r < 0) return r;
    attrs = GetFileAttributesW(wide);
    if (attrs == INVALID_FILE_ATTRIBUTES) return nt_last_error();
    if ((mode & 2) && (attrs & FILE_ATTRIBUTE_READONLY)) return -NT_EACCES;
    if ((mode & 1) && !(attrs & FILE_ATTRIBUTE_DIRECTORY) &&
        !wide_suffix_ci(wide, L".exe") && !wide_suffix_ci(wide, L".com") &&
        !wide_suffix_ci(wide, L".bat") && !wide_suffix_ci(wide, L".cmd"))
        return -NT_EACCES;
    return 0;
}
