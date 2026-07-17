/*
 * Linux x86-64 ABI seen by musl-nt.
 *
 * This header deliberately does not include musl or mingw headers.  The NT
 * backend consumes Linux syscall arguments, so its structures and constants
 * must not accidentally follow the host compiler's CRT ABI.
 */
#ifndef MUSL_NT_ABI_H
#define MUSL_NT_ABI_H

#include <stdint.h>
#include <stddef.h>

/* Syscall register width.  This must stay 64-bit even when the NT-facing
 * backend is compiled by an LLP64 MinGW compiler. */
typedef int64_t nt_sc_t;

/* Linux errno values. */
#define NT_EPERM            1
#define NT_ENOENT           2
#define NT_ESRCH            3
#define NT_EINTR            4
#define NT_EIO              5
#define NT_ENXIO            6
#define NT_E2BIG            7
#define NT_ENOEXEC          8
#define NT_EBADF            9
#define NT_ECHILD          10
#define NT_EAGAIN          11
#define NT_ENOMEM          12
#define NT_EACCES          13
#define NT_EFAULT          14
#define NT_EBUSY           16
#define NT_EEXIST          17
#define NT_EXDEV           18
#define NT_ENODEV          19
#define NT_ENOTDIR         20
#define NT_EISDIR          21
#define NT_EINVAL          22
#define NT_ENFILE          23
#define NT_EMFILE          24
#define NT_ENOTTY          25
#define NT_EFBIG           27
#define NT_ENOSPC          28
#define NT_ESPIPE          29
#define NT_EROFS           30
#define NT_EMLINK          31
#define NT_EPIPE           32
#define NT_ERANGE          34
#define NT_ENAMETOOLONG    36
#define NT_ENOSYS          38
#define NT_ENOTEMPTY       39
#define NT_ELOOP           40
#define NT_ENOMSG          42
#define NT_EOVERFLOW       75
#define NT_ENOTSOCK        88
#define NT_EDESTADDRREQ    89
#define NT_EMSGSIZE        90
#define NT_EPROTONOSUPPORT 93
#define NT_EAFNOSUPPORT    97
#define NT_EADDRINUSE      98
#define NT_EADDRNOTAVAIL   99
#define NT_ENETDOWN       100
#define NT_ENETUNREACH    101
#define NT_ECONNABORTED   103
#define NT_ECONNRESET     104
#define NT_ENOBUFS        105
#define NT_EISCONN        106
#define NT_ENOTCONN       107
#define NT_ETIMEDOUT      110
#define NT_ECONNREFUSED   111
#define NT_EHOSTUNREACH   113
#define NT_EALREADY       114
#define NT_EINPROGRESS    115
#define NT_ECANCELED      125
#define NT_ENOTSUP        95

/* Linux x86-64 syscall numbers used by the selected musl closure. */
#define NT_SYS_read                 0
#define NT_SYS_write                1
#define NT_SYS_open                 2
#define NT_SYS_close                3
#define NT_SYS_stat                 4
#define NT_SYS_fstat                5
#define NT_SYS_lstat                6
#define NT_SYS_poll                 7
#define NT_SYS_lseek                8
#define NT_SYS_mmap                 9
#define NT_SYS_mprotect            10
#define NT_SYS_munmap              11
#define NT_SYS_brk                 12
#define NT_SYS_rt_sigaction        13
#define NT_SYS_rt_sigprocmask      14
#define NT_SYS_ioctl               16
#define NT_SYS_pread64             17
#define NT_SYS_pwrite64            18
#define NT_SYS_readv               19
#define NT_SYS_writev              20
#define NT_SYS_access              21
#define NT_SYS_pipe                22
#define NT_SYS_sched_yield         24
#define NT_SYS_msync               26
#define NT_SYS_madvise             28
#define NT_SYS_dup                 32
#define NT_SYS_dup2                33
#define NT_SYS_nanosleep           35
#define NT_SYS_alarm               37
#define NT_SYS_getpid              39
#define NT_SYS_socket              41
#define NT_SYS_connect             42
#define NT_SYS_accept              43
#define NT_SYS_sendto              44
#define NT_SYS_recvfrom            45
#define NT_SYS_sendmsg             46
#define NT_SYS_recvmsg             47
#define NT_SYS_shutdown            48
#define NT_SYS_bind                49
#define NT_SYS_listen              50
#define NT_SYS_getsockname         51
#define NT_SYS_getpeername         52
#define NT_SYS_socketpair          53
#define NT_SYS_setsockopt          54
#define NT_SYS_getsockopt          55
#define NT_SYS_fork                57
#define NT_SYS_vfork               58
#define NT_SYS_execve              59
#define NT_SYS_exit                60
#define NT_SYS_wait4               61
#define NT_SYS_kill                62
#define NT_SYS_uname               63
#define NT_SYS_fcntl               72
#define NT_SYS_fsync               74
#define NT_SYS_fdatasync           75
#define NT_SYS_truncate            76
#define NT_SYS_ftruncate           77
#define NT_SYS_getdents            78
#define NT_SYS_getcwd              79
#define NT_SYS_chdir               80
#define NT_SYS_fchdir              81
#define NT_SYS_rename              82
#define NT_SYS_mkdir               83
#define NT_SYS_rmdir               84
#define NT_SYS_link                86
#define NT_SYS_unlink              87
#define NT_SYS_symlink             88
#define NT_SYS_readlink            89
#define NT_SYS_chmod               90
#define NT_SYS_fchmod              91
#define NT_SYS_chown               92
#define NT_SYS_fchown              93
#define NT_SYS_lchown              94
#define NT_SYS_umask               95
#define NT_SYS_gettimeofday        96
#define NT_SYS_getrlimit           97
#define NT_SYS_getuid             102
#define NT_SYS_getgid             104
#define NT_SYS_setuid             105
#define NT_SYS_setgid             106
#define NT_SYS_geteuid            107
#define NT_SYS_getegid            108
#define NT_SYS_setpgid            109
#define NT_SYS_getppid            110
#define NT_SYS_getpgrp            111
#define NT_SYS_setsid             112
#define NT_SYS_setreuid           113
#define NT_SYS_setregid           114
#define NT_SYS_getgroups          115
#define NT_SYS_setgroups          116
#define NT_SYS_setresuid          117
#define NT_SYS_getresuid          118
#define NT_SYS_setresgid          119
#define NT_SYS_getresgid          120
#define NT_SYS_getpgid            121
#define NT_SYS_getsid             124
#define NT_SYS_rt_sigpending      127
#define NT_SYS_rt_sigsuspend      130
#define NT_SYS_sigaltstack        131
#define NT_SYS_statfs             137
#define NT_SYS_fstatfs            138
#define NT_SYS_getpriority        140
#define NT_SYS_setpriority        141
#define NT_SYS_sync               162
#define NT_SYS_gettid             186
#define NT_SYS_tkill              200
#define NT_SYS_time               201
#define NT_SYS_tgkill             234
#define NT_SYS_sched_getaffinity  204
#define NT_SYS_futex              202
#define NT_SYS_getdents64         217
#define NT_SYS_set_tid_address    218
#define NT_SYS_clock_settime      227
#define NT_SYS_clock_gettime      228
#define NT_SYS_clock_getres       229
#define NT_SYS_clock_nanosleep    230
#define NT_SYS_exit_group         231
#define NT_SYS_openat             257
#define NT_SYS_mkdirat            258
#define NT_SYS_mknodat            259
#define NT_SYS_fchownat           260
#define NT_SYS_newfstatat         262
#define NT_SYS_unlinkat           263
#define NT_SYS_renameat           264
#define NT_SYS_linkat             265
#define NT_SYS_symlinkat          266
#define NT_SYS_readlinkat         267
#define NT_SYS_fchmodat           268
#define NT_SYS_faccessat          269
#define NT_SYS_ppoll              271
#define NT_SYS_set_robust_list    273
#define NT_SYS_utimensat          280
#define NT_SYS_accept4            288
#define NT_SYS_dup3               292
#define NT_SYS_pipe2              293
#define NT_SYS_preadv             295
#define NT_SYS_pwritev            296
#define NT_SYS_recvmmsg           299
#define NT_SYS_prlimit64          302
#define NT_SYS_sendmmsg           307
#define NT_SYS_syncfs             306
#define NT_SYS_renameat2          316
#define NT_SYS_getrandom          318
#define NT_SYS_execveat           322
#define NT_SYS_preadv2            327
#define NT_SYS_pwritev2           328
#define NT_SYS_statx              332
#define NT_SYS_faccessat2         439
#define NT_SYS_fchmodat2          452

/* Pseudo-syscall interna da NTUnix (fora da faixa Linux): posix_spawn faz
 * CreateProcessW no processo atual e devolve o pid. Ver override/posix_spawn.c
 * e nt_sys_spawn (sys_proc.c). */
#define NT_SYS_spawn              0x1000000

/* Linux fcntl/open/at constants. */
#define NT_AT_FDCWD              -100
#define NT_AT_SYMLINK_NOFOLLOW 0x0100
#define NT_AT_REMOVEDIR        0x0200
#define NT_AT_SYMLINK_FOLLOW   0x0400
#define NT_AT_EMPTY_PATH       0x1000

#define NT_O_ACCMODE        00000003
#define NT_O_RDONLY         00000000
#define NT_O_WRONLY         00000001
#define NT_O_RDWR           00000002
#define NT_O_CREAT          00000100
#define NT_O_EXCL           00000200
#define NT_O_NOCTTY         00000400
#define NT_O_TRUNC          00001000
#define NT_O_APPEND         00002000
#define NT_O_NONBLOCK       00004000
#define NT_O_DIRECTORY      00200000
#define NT_O_NOFOLLOW       00400000
#define NT_O_CLOEXEC        02000000
#define NT_O_PATH          010000000

#define NT_F_DUPFD       0
#define NT_F_GETFD       1
#define NT_F_SETFD       2
#define NT_F_GETFL       3
#define NT_F_SETFL       4
#define NT_F_DUPFD_CLOEXEC 1030
#define NT_FD_CLOEXEC    1

#define NT_SEEK_SET 0
#define NT_SEEK_CUR 1
#define NT_SEEK_END 2

#define NT_PROT_NONE  0
#define NT_PROT_READ  1
#define NT_PROT_WRITE 2
#define NT_PROT_EXEC  4
#define NT_MAP_SHARED    0x01
#define NT_MAP_PRIVATE   0x02
#define NT_MAP_FIXED     0x10
#define NT_MAP_ANONYMOUS 0x20
#define NT_MAP_FIXED_NOREPLACE 0x100000
#define NT_MS_ASYNC 1
#define NT_MS_SYNC  4

#define NT_S_IFMT  0170000
#define NT_S_IFIFO 0010000
#define NT_S_IFCHR 0020000
#define NT_S_IFDIR 0040000
#define NT_S_IFREG 0100000
#define NT_S_IFLNK 0120000
#define NT_S_IRUSR 0400
#define NT_S_IWUSR 0200
#define NT_S_IXUSR 0100
#define NT_S_IRGRP 0040
#define NT_S_IWGRP 0020
#define NT_S_IXGRP 0010
#define NT_S_IROTH 0004
#define NT_S_IWOTH 0002
#define NT_S_IXOTH 0001

#define NT_DT_UNKNOWN 0
#define NT_DT_FIFO    1
#define NT_DT_CHR     2
#define NT_DT_DIR     4
#define NT_DT_REG     8
#define NT_DT_LNK    10

#define NT_STATX_BASIC_STATS 0x07ffU
#define NT_STATX_BTIME       0x0800U

#define NT_CLOCK_REALTIME           0
#define NT_CLOCK_MONOTONIC          1
#define NT_CLOCK_PROCESS_CPUTIME_ID 2
#define NT_CLOCK_THREAD_CPUTIME_ID  3
#define NT_TIMER_ABSTIME            1

#define NT_POLLIN   0x001
#define NT_POLLOUT  0x004
#define NT_POLLERR  0x008
#define NT_POLLHUP  0x010
#define NT_POLLNVAL 0x020

#define NT_AF_UNIX   1
#define NT_AF_INET   2
#define NT_AF_INET6 10

#define NT_SOCK_STREAM    1
#define NT_SOCK_DGRAM     2
#define NT_SOCK_NONBLOCK  0x800
#define NT_SOCK_CLOEXEC   0x80000

#define NT_TIOCGWINSZ 0x5413UL
#define NT_TCGETS     0x5401UL

struct nt_iovec {
    void *iov_base;
    uint64_t iov_len;
};

struct nt_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

struct nt_timeval {
    int64_t tv_sec;
    int64_t tv_usec;
};

struct nt_timezone {
    int32_t tz_minuteswest;
    int32_t tz_dsttime;
};

struct nt_winsize {
    uint16_t ws_row, ws_col, ws_xpixel, ws_ypixel;
};

struct nt_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

struct nt_msghdr {
    void *msg_name;
    uint32_t msg_namelen;
    uint32_t __pad1;
    struct nt_iovec *msg_iov;
    uint64_t msg_iovlen;
    void *msg_control;
    uint64_t msg_controllen;
    int32_t msg_flags;
    int32_t __pad2;
};

struct nt_mmsghdr {
    struct nt_msghdr msg_hdr;
    uint32_t msg_len;
    uint32_t __pad;
};

struct nt_statx_timestamp {
    int64_t tv_sec;
    uint32_t tv_nsec;
    int32_t __pad;
};

struct nt_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __pad0;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct nt_statx_timestamp stx_atime;
    struct nt_statx_timestamp stx_btime;
    struct nt_statx_timestamp stx_ctime;
    struct nt_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t __spare[14];
};

/* Kernel dirent layout consumed directly by musl's readdir. */
struct nt_linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[];
};

struct nt_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
};

struct nt_rlimit {
    uint64_t rlim_cur;
    uint64_t rlim_max;
};

/* Linux x86-64 struct statfs. */
struct nt_statfs {
    uint64_t f_type;
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    uint64_t f_namelen;
    uint64_t f_frsize;
    uint64_t f_flags;
    uint64_t f_spare[4];
};

#define NT_UTIME_NOW  0x3fffffffL
#define NT_UTIME_OMIT 0x3ffffffeL

/* Layout of Linux x86-64 struct stat, used only by legacy syscall fallbacks. */
struct nt_stat {
    uint64_t st_dev, st_ino, st_nlink;
    uint32_t st_mode, st_uid, st_gid, __pad0;
    uint64_t st_rdev;
    int64_t st_size, st_blksize, st_blocks;
    struct nt_timespec st_atim, st_mtim, st_ctim;
    int64_t __unused[3];
};

#endif
