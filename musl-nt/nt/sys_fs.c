#include "nt/ntpriv.h"

#define NT_NTFS_SUPER_MAGIC 0x5346544eULL
#define NT_ST_RDONLY 1ULL

static nt_sc_t statfs_wide(const WCHAR *path, struct nt_statfs *out)
{
    WCHAR volume[NT_PATH_CAP];
    WCHAR filesystem[64];
    ULARGE_INTEGER available, total, free_bytes;
    DWORD serial = 0, maximum_name = 255, volume_flags = 0;
    DWORD sectors_per_cluster, bytes_per_sector;
    DWORD free_clusters, total_clusters;
    uint64_t block_size = 4096;

    if (!out) return -NT_EFAULT;
    if (!GetVolumePathNameW(path, volume, NT_ARRAY_LEN(volume)))
        return nt_last_error();
    if (GetDiskFreeSpaceW(volume, &sectors_per_cluster, &bytes_per_sector,
                          &free_clusters, &total_clusters) &&
        sectors_per_cluster && bytes_per_sector)
        block_size = (uint64_t)sectors_per_cluster * bytes_per_sector;
    if (!GetDiskFreeSpaceExW(volume, &available, &total, &free_bytes))
        return nt_last_error();
    filesystem[0] = 0;
    if (!GetVolumeInformationW(volume, 0, 0, &serial, &maximum_name,
                               &volume_flags, filesystem,
                               NT_ARRAY_LEN(filesystem)))
        return nt_last_error();

    nt_memset(out, 0, sizeof *out);
    out->f_type = NT_NTFS_SUPER_MAGIC;
    out->f_bsize = block_size;
    out->f_frsize = block_size;
    out->f_blocks = total.QuadPart / block_size;
    out->f_bfree = free_bytes.QuadPart / block_size;
    out->f_bavail = available.QuadPart / block_size;
    out->f_fsid[0] = (int32_t)serial;
    out->f_fsid[1] = (int32_t)(serial ^ 0x4e54554eU);
    out->f_namelen = maximum_name;
    if (volume_flags & FILE_READ_ONLY_VOLUME) out->f_flags |= NT_ST_RDONLY;
    return 0;
}

nt_sc_t nt_sys_statfs(nt_sc_t path_arg, nt_sc_t out_arg)
{
    WCHAR path[NT_PATH_CAP];
    nt_sc_t r;
    if (!path_arg) return -NT_EFAULT;
    r = nt_path_at(NT_AT_FDCWD, (const char *)(uintptr_t)path_arg,
                   path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    return statfs_wide(path, (struct nt_statfs *)(uintptr_t)out_arg);
}

nt_sc_t nt_sys_fstatfs(nt_sc_t fd, nt_sc_t out_arg)
{
    HANDLE handle = nt_fd_handle((int)fd);
    WCHAR path[NT_PATH_CAP];
    DWORD n;
    if (handle == INVALID_HANDLE_VALUE) return -NT_EBADF;
    n = GetFinalPathNameByHandleW(handle, path, NT_ARRAY_LEN(path),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return nt_last_error();
    if (n >= NT_ARRAY_LEN(path)) return -NT_ENAMETOOLONG;
    return statfs_wide(path, (struct nt_statfs *)(uintptr_t)out_arg);
}

nt_sc_t nt_sys_chdir(nt_sc_t path_arg)
{
    WCHAR path[NT_PATH_CAP];
    DWORD attrs;
    nt_sc_t r = nt_path_at(NT_AT_FDCWD, (const char *)(uintptr_t)path_arg,
                        path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    attrs = GetFileAttributesW(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return nt_last_error();
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) return -NT_ENOTDIR;
    if (!SetCurrentDirectoryW(path)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_fchdir(nt_sc_t fd)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    WCHAR path[NT_PATH_CAP];
    DWORD n;
    if (!slot) return -NT_EBADF;
    if (slot->kind != NT_FD_DIR) return -NT_ENOTDIR;
    n = GetFinalPathNameByHandleW(slot->handle, path, NT_ARRAY_LEN(path),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return nt_last_error();
    if (n >= NT_ARRAY_LEN(path)) return -NT_ENAMETOOLONG;
    if (!SetCurrentDirectoryW(path)) return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_getcwd(nt_sc_t buf_arg, nt_sc_t size_arg)
{
    char *buf = (char *)(uintptr_t)buf_arg;
    WCHAR path[NT_PATH_CAP];
    DWORD n;
    if (!buf || size_arg <= 0) return -NT_EINVAL;
    n = GetCurrentDirectoryW(NT_ARRAY_LEN(path), path);
    if (!n) return nt_last_error();
    if (n >= NT_ARRAY_LEN(path)) return -NT_ENAMETOOLONG;
    return nt_path_to_unix(path, buf, (size_t)size_arg);
}

nt_sc_t nt_sys_fsync(nt_sc_t fd)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    WCHAR path[NT_PATH_CAP];
    HANDLE writable;
    DWORD n, error;
    if (!slot) return -NT_EBADF;
    if (slot->kind == NT_FD_CONSOLE || slot->kind == NT_FD_PIPE) return 0;
    if (FlushFileBuffers(slot->handle)) return 0;

    error = GetLastError();
    if (error != ERROR_ACCESS_DENIED ||
        (slot->flags & NT_O_ACCMODE) != NT_O_RDONLY)
        return nt_error(error);

    /* Win32 requires GENERIC_WRITE for FlushFileBuffers, unlike Linux where
     * fsync on an O_RDONLY descriptor is useful (and used by BusyBox fsync).
     * Reopen the same object only for the duration of the flush. */
    n = GetFinalPathNameByHandleW(slot->handle, path, NT_ARRAY_LEN(path),
                                  FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return nt_last_error();
    if (n >= NT_ARRAY_LEN(path)) return -NT_ENAMETOOLONG;
    writable = CreateFileW(path, GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE |
                           FILE_SHARE_DELETE, 0, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (writable == INVALID_HANDLE_VALUE) return nt_last_error();
    if (!FlushFileBuffers(writable)) {
        nt_sc_t r = nt_last_error();
        CloseHandle(writable);
        return r;
    }
    CloseHandle(writable);
    return 0;
}

nt_sc_t nt_sys_sync(void)
{
    /* NT has no process-scoped equivalent of Linux sync(2).  Individual
     * fsync/fdatasync calls are real; global sync is advisory in v0. */
    return 0;
}
