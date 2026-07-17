#include "nt/ntpriv.h"

struct nt_reparse_buffer {
    ULONG tag;
    USHORT data_length;
    USHORT reserved;
    union {
        struct {
            USHORT substitute_offset;
            USHORT substitute_length;
            USHORT print_offset;
            USHORT print_length;
            ULONG flags;
            WCHAR path[1];
        } symlink;
        struct {
            USHORT substitute_offset;
            USHORT substitute_length;
            USHORT print_offset;
            USHORT print_length;
            WCHAR path[1];
        } mount;
        unsigned char raw[1];
    } value;
};

nt_sc_t nt_sys_readlinkat(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t buf_arg, nt_sc_t count_arg)
{
    WCHAR path[NT_PATH_CAP];
    unsigned char raw[MAXIMUM_REPARSE_DATA_BUFFER_SIZE];
    struct nt_reparse_buffer *rp = (void *)raw;
    WCHAR target[NT_PATH_CAP];
    const WCHAR *source;
    size_t source_len, copied;
    USHORT offset, byte_len;
    DWORD returned;
    HANDLE h;
    char *out = (char *)(uintptr_t)buf_arg;
    nt_sc_t r;
    if (!out && count_arg) return -NT_EFAULT;
    if (count_arg < 0) return -NT_EINVAL;
    r = nt_path_at((int)dirfd, (const char *)(uintptr_t)path_arg,
                   path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    h = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, OPEN_EXISTING,
                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                    0);
    if (h == INVALID_HANDLE_VALUE) return nt_last_error();
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, 0, 0, raw, sizeof raw,
                         &returned, 0)) {
        r = nt_last_error();
        CloseHandle(h);
        return r;
    }
    CloseHandle(h);
    if (rp->tag == IO_REPARSE_TAG_SYMLINK) {
        offset = rp->value.symlink.print_length
                   ? rp->value.symlink.print_offset
                   : rp->value.symlink.substitute_offset;
        byte_len = rp->value.symlink.print_length
                    ? rp->value.symlink.print_length
                    : rp->value.symlink.substitute_length;
        source = rp->value.symlink.path + offset / sizeof(WCHAR);
    } else if (rp->tag == IO_REPARSE_TAG_MOUNT_POINT) {
        offset = rp->value.mount.print_length
                   ? rp->value.mount.print_offset
                   : rp->value.mount.substitute_offset;
        byte_len = rp->value.mount.print_length
                    ? rp->value.mount.print_length
                    : rp->value.mount.substitute_length;
        source = rp->value.mount.path + offset / sizeof(WCHAR);
    } else {
        return -NT_EINVAL;
    }
    source_len = byte_len / sizeof(WCHAR);
    if (source_len >= NT_ARRAY_LEN(target)) return -NT_ENAMETOOLONG;
    nt_memcpy(target, source, source_len * sizeof(WCHAR));
    target[source_len] = 0;
    if (source_len >= 4 && target[0] == L'\\' && target[1] == L'?' &&
        target[2] == L'?' && target[3] == L'\\') {
        size_t i;
        for (i = 4; i <= source_len; ++i) target[i - 4] = target[i];
    }
    {
        char utf8[NT_PATH_CAP];
        r = nt_path_to_unix(target, utf8, sizeof utf8);
        if (r < 0) return r;
        copied = (size_t)(r - 1);
        if (copied > (size_t)count_arg) copied = (size_t)count_arg;
        nt_memcpy(out, utf8, copied);
    }
    return (nt_sc_t)copied;
}
