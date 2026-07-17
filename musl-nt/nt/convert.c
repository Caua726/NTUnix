#include "nt/ntpriv.h"

#define NT_EPOCH_100NS 116444736000000000ULL

static FILETIME filetime_from_large_integer(LARGE_INTEGER value)
{
    ULARGE_INTEGER bits;
    FILETIME result;
    bits.QuadPart = (uint64_t)value.QuadPart;
    result.dwLowDateTime = bits.LowPart;
    result.dwHighDateTime = bits.HighPart;
    return result;
}

void nt_filetime_to_timespec(FILETIME ft, struct nt_timespec *out)
{
    ULARGE_INTEGER value;
    uint64_t unix_ticks;
    value.LowPart = ft.dwLowDateTime;
    value.HighPart = ft.dwHighDateTime;
    if (value.QuadPart < NT_EPOCH_100NS) {
        out->tv_sec = 0;
        out->tv_nsec = 0;
        return;
    }
    unix_ticks = value.QuadPart - NT_EPOCH_100NS;
    out->tv_sec = (int64_t)(unix_ticks / 10000000ULL);
    out->tv_nsec = (int64_t)((unix_ticks % 10000000ULL) * 100ULL);
}

FILETIME nt_timespec_to_filetime(const struct nt_timespec *ts)
{
    ULARGE_INTEGER value;
    FILETIME ft;
    value.QuadPart = NT_EPOCH_100NS + (uint64_t)ts->tv_sec * 10000000ULL +
                     (uint64_t)ts->tv_nsec / 100ULL;
    ft.dwLowDateTime = value.LowPart;
    ft.dwHighDateTime = value.HighPart;
    return ft;
}

uint8_t nt_dtype_from_attributes(DWORD attrs, ULONG reparse_tag)
{
    if ((attrs & FILE_ATTRIBUTE_REPARSE_POINT) &&
        (reparse_tag == IO_REPARSE_TAG_SYMLINK ||
         reparse_tag == IO_REPARSE_TAG_MOUNT_POINT))
        return NT_DT_LNK;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) return NT_DT_DIR;
    if (attrs & (FILE_ATTRIBUTE_DEVICE | FILE_ATTRIBUTE_REPARSE_POINT))
        return NT_DT_UNKNOWN;
    return NT_DT_REG;
}

nt_sc_t nt_stat_from_handle(HANDLE h, struct nt_statx *out)
{
    BY_HANDLE_FILE_INFORMATION old;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    FILE_ID_INFO id;
    DWORD type;
    uint16_t perms;
    if (!h || h == INVALID_HANDLE_VALUE || !out) return -NT_EBADF;
    nt_memset(out, 0, sizeof *out);
    type = GetFileType(h);
    if (type == FILE_TYPE_CHAR || type == FILE_TYPE_PIPE) {
        out->stx_mask = NT_STATX_BASIC_STATS;
        out->stx_blksize = 4096;
        out->stx_nlink = 1;
        out->stx_mode = (type == FILE_TYPE_PIPE ? NT_S_IFIFO : NT_S_IFCHR) | 0666;
        return 0;
    }
    if (!GetFileInformationByHandleEx(h, FileBasicInfo, &basic, sizeof basic))
        return nt_last_error();
    if (!GetFileInformationByHandleEx(h, FileStandardInfo, &standard,
                                      sizeof standard))
        return nt_last_error();

    out->stx_mask = NT_STATX_BASIC_STATS | NT_STATX_BTIME;
    out->stx_blksize = 4096;
    out->stx_nlink = standard.NumberOfLinks ? standard.NumberOfLinks : 1;
    perms = (basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 0755 : 0644;
    if (basic.FileAttributes & FILE_ATTRIBUTE_READONLY)
        perms &= (uint16_t)~0222;
    out->stx_mode = (uint16_t)((basic.FileAttributes & FILE_ATTRIBUTE_DIRECTORY
                               ? NT_S_IFDIR : NT_S_IFREG) | perms);
    out->stx_size = (uint64_t)standard.EndOfFile.QuadPart;
    out->stx_blocks = (out->stx_size + 511) / 512;
    nt_filetime_to_timespec(filetime_from_large_integer(basic.LastAccessTime),
                            (struct nt_timespec *)(void *)&out->stx_atime);
    nt_filetime_to_timespec(filetime_from_large_integer(basic.CreationTime),
                            (struct nt_timespec *)(void *)&out->stx_btime);
    nt_filetime_to_timespec(filetime_from_large_integer(basic.ChangeTime),
                            (struct nt_timespec *)(void *)&out->stx_ctime);
    nt_filetime_to_timespec(filetime_from_large_integer(basic.LastWriteTime),
                            (struct nt_timespec *)(void *)&out->stx_mtime);
    if (GetFileInformationByHandleEx(h, FileIdInfo, &id, sizeof id)) {
        out->stx_dev_major = id.VolumeSerialNumber;
        nt_memcpy(&out->stx_ino, id.FileId.Identifier,
                  sizeof out->stx_ino);
    } else if (GetFileInformationByHandle(h, &old)) {
        out->stx_dev_major = old.dwVolumeSerialNumber;
        out->stx_ino = ((uint64_t)old.nFileIndexHigh << 32) |
                       old.nFileIndexLow;
    }
    return 0;
}

void nt_statx_to_stat(const struct nt_statx *src, struct nt_stat *dst)
{
    nt_memset(dst, 0, sizeof *dst);
    dst->st_dev = src->stx_dev_major;
    dst->st_ino = src->stx_ino;
    dst->st_nlink = src->stx_nlink;
    dst->st_mode = src->stx_mode;
    dst->st_uid = src->stx_uid;
    dst->st_gid = src->stx_gid;
    dst->st_rdev = src->stx_rdev_major;
    dst->st_size = (int64_t)src->stx_size;
    dst->st_blksize = src->stx_blksize;
    dst->st_blocks = (int64_t)src->stx_blocks;
    dst->st_atim.tv_sec = src->stx_atime.tv_sec;
    dst->st_atim.tv_nsec = src->stx_atime.tv_nsec;
    dst->st_mtim.tv_sec = src->stx_mtime.tv_sec;
    dst->st_mtim.tv_nsec = src->stx_mtime.tv_nsec;
    dst->st_ctim.tv_sec = src->stx_ctime.tv_sec;
    dst->st_ctim.tv_nsec = src->stx_ctime.tv_nsec;
}
