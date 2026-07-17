/* Internal contract shared by the NT syscall backends. */
#ifndef MUSL_NT_PRIV_H
#define MUSL_NT_PRIV_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winioctl.h>
#include <winternl.h>
#include "ntabi.h"

#if defined(__x86_64__) && defined(__GNUC__)
#define NT_SYSV_ABI __attribute__((sysv_abi))
#else
#define NT_SYSV_ABI
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define NT_ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
#define NT_MAX_FDS 1024
#define NT_PATH_CAP 32768

enum nt_fd_kind {
    NT_FD_NONE = 0,
    NT_FD_FILE,
    NT_FD_DIR,
    NT_FD_PIPE,
    NT_FD_CONSOLE,
    NT_FD_SOCKET
};

/* Dispositivos sintéticos de /dev que não têm handle NT equivalente. O slot
 * recebe um handle real (NUL) para ser válido/fechável; read/write intercepta
 * pelo devkind antes de tocar o handle. NT_DEV_NONE = arquivo/pipe normal. */
#define NT_DEV_NONE   0
#define NT_DEV_ZERO   1  /* /dev/zero: lê zeros, escrita descartada */
#define NT_DEV_FULL   2  /* /dev/full: lê zeros, escrita -> ENOSPC */
#define NT_DEV_RANDOM 3  /* /dev/random,/dev/urandom: lê bytes aleatórios */

struct nt_fd {
    HANDLE handle;
    enum nt_fd_kind kind;
    int flags;
    int fd_flags;
    int devkind;
    int dir_eof;
    uint64_t dir_cookie;
    SRWLOCK io_lock;
};

/* errno_xlat.c */
int nt_errno_from_win32(DWORD error);
int nt_errno_from_wsa(int error);
nt_sc_t nt_error(DWORD error);
nt_sc_t nt_last_error(void);
nt_sc_t nt_wsa_error(void);
nt_sc_t nt_error_from_status(NTSTATUS status);

/* fdtable.c: returned slots remain stable; callers must lock io_lock when
 * mutating the underlying file pointer or directory enumeration state. */
void nt_fdtable_init(void);
struct nt_fd *nt_fd_get(int fd);
HANDLE nt_fd_handle(int fd);
int nt_fd_alloc(HANDLE handle, enum nt_fd_kind kind, int flags, int fd_flags,
                int minimum);
nt_sc_t nt_fd_install_at(int fd, HANDLE handle, enum nt_fd_kind kind, int flags,
                      int fd_flags);
nt_sc_t nt_fd_close(int fd);
void nt_fd_reattach_console(void);
void nt_fd_make_inheritable(void);
nt_sc_t nt_fd_dup(int oldfd, int minimum, int cloexec);
nt_sc_t nt_fd_dup2(int oldfd, int newfd, int cloexec);
void nt_fd_set_devkind(int fd, int devkind);

/* ntpath.c */
size_t nt_strlen(const char *s);
size_t nt_wcslen(const WCHAR *s);
void *nt_memcpy(void *dst, const void *src, size_t n);
void *nt_memset(void *dst, int value, size_t n);
int nt_streq(const char *a, const char *b);
nt_sc_t nt_utf8_to_wide(const char *src, WCHAR *dst, size_t cap);
nt_sc_t nt_wide_to_utf8(const WCHAR *src, size_t len, char *dst, size_t cap,
                     size_t *written);
nt_sc_t nt_path_at(int dirfd, const char *path, WCHAR *out, size_t cap);
nt_sc_t nt_path_to_unix(const WCHAR *path, char *out, size_t cap);

/* convert.c */
void nt_filetime_to_timespec(FILETIME ft, struct nt_timespec *out);
FILETIME nt_timespec_to_filetime(const struct nt_timespec *ts);
nt_sc_t nt_stat_from_handle(HANDLE handle, struct nt_statx *out);
void nt_statx_to_stat(const struct nt_statx *src, struct nt_stat *dst);
uint8_t nt_dtype_from_attributes(DWORD attrs, ULONG reparse_tag);

/* syscall backends */
nt_sc_t nt_sys_openat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t flags, nt_sc_t mode);
nt_sc_t nt_sys_read(nt_sc_t fd, nt_sc_t buf, nt_sc_t count);
nt_sc_t nt_sys_write(nt_sc_t fd, nt_sc_t buf, nt_sc_t count);
nt_sc_t nt_sys_readv(nt_sc_t fd, nt_sc_t iov, nt_sc_t count);
nt_sc_t nt_sys_writev(nt_sc_t fd, nt_sc_t iov, nt_sc_t count);
nt_sc_t nt_sys_close(nt_sc_t fd);
nt_sc_t nt_sys_lseek(nt_sc_t fd, nt_sc_t offset, nt_sc_t whence);
nt_sc_t nt_sys_dup(nt_sc_t fd);
nt_sc_t nt_sys_dup2(nt_sc_t oldfd, nt_sc_t newfd);
nt_sc_t nt_sys_dup3(nt_sc_t oldfd, nt_sc_t newfd, nt_sc_t flags);
nt_sc_t nt_sys_fcntl(nt_sc_t fd, nt_sc_t cmd, nt_sc_t arg);
nt_sc_t nt_sys_ftruncate(nt_sc_t fd, nt_sc_t length);
nt_sc_t nt_sys_truncate(nt_sc_t path, nt_sc_t length);
nt_sc_t nt_sys_pread(nt_sc_t fd, nt_sc_t buf, nt_sc_t count, nt_sc_t offset);
nt_sc_t nt_sys_pwrite(nt_sc_t fd, nt_sc_t buf, nt_sc_t count, nt_sc_t offset);
nt_sc_t nt_sys_pipe2(nt_sc_t pipefd, nt_sc_t flags);
nt_sc_t nt_sys_poll(nt_sc_t fds, nt_sc_t nfds, nt_sc_t timeout);
int nt_socket_close(HANDLE handle);
nt_sc_t nt_socket_duplicate(HANDLE handle, int cloexec, HANDLE *copy);
nt_sc_t nt_socket_read(struct nt_fd *slot, void *buffer, uint64_t length);
nt_sc_t nt_socket_write(struct nt_fd *slot, const void *buffer,
                        uint64_t length);
int nt_socket_poll(struct nt_fd *slot, short events, short *revents);
nt_sc_t nt_socket_set_nonblocking(struct nt_fd *slot, int enabled);

nt_sc_t nt_sys_mmap(nt_sc_t addr, nt_sc_t length, nt_sc_t prot, nt_sc_t flags, nt_sc_t fd, nt_sc_t off);
nt_sc_t nt_sys_munmap(nt_sc_t addr, nt_sc_t length);
nt_sc_t nt_sys_mprotect(nt_sc_t addr, nt_sc_t length, nt_sc_t prot);
nt_sc_t nt_sys_msync(nt_sc_t addr, nt_sc_t length, nt_sc_t flags);
nt_sc_t nt_sys_madvise(nt_sc_t addr, nt_sc_t length, nt_sc_t advice);
nt_sc_t nt_sys_brk(nt_sc_t addr);

nt_sc_t nt_sys_statx(nt_sc_t dirfd, nt_sc_t path, nt_sc_t flags, nt_sc_t mask, nt_sc_t out);
nt_sc_t nt_sys_fstat(nt_sc_t fd, nt_sc_t out);
nt_sc_t nt_sys_newfstatat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t out, nt_sc_t flags);
nt_sc_t nt_sys_faccessat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t mode, nt_sc_t flags);

nt_sc_t nt_sys_getdents64(nt_sc_t fd, nt_sc_t buf, nt_sc_t count);
nt_sc_t nt_sys_mkdirat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t mode);
nt_sc_t nt_sys_unlinkat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t flags);
nt_sc_t nt_sys_renameat2(nt_sc_t olddirfd, nt_sc_t oldpath, nt_sc_t newdirfd,
                      nt_sc_t newpath, nt_sc_t flags);
nt_sc_t nt_sys_readlinkat(nt_sc_t dirfd, nt_sc_t path, nt_sc_t buf, nt_sc_t count);
nt_sc_t nt_sys_linkat(nt_sc_t olddirfd, nt_sc_t oldpath, nt_sc_t newdirfd,
                   nt_sc_t newpath, nt_sc_t flags);
nt_sc_t nt_sys_symlinkat(nt_sc_t target, nt_sc_t newdirfd, nt_sc_t newpath);

nt_sc_t nt_sys_chdir(nt_sc_t path);
nt_sc_t nt_sys_fchdir(nt_sc_t fd);
nt_sc_t nt_sys_getcwd(nt_sc_t buf, nt_sc_t size);
nt_sc_t nt_sys_fsync(nt_sc_t fd);
nt_sc_t nt_sys_sync(void);
nt_sc_t nt_sys_statfs(nt_sc_t path, nt_sc_t out);
nt_sc_t nt_sys_fstatfs(nt_sc_t fd, nt_sc_t out);

nt_sc_t nt_sys_socket(nt_sc_t domain, nt_sc_t type, nt_sc_t protocol);
nt_sc_t nt_sys_socketpair(nt_sc_t domain, nt_sc_t type, nt_sc_t protocol,
                          nt_sc_t sockets);
nt_sc_t nt_sys_bind(nt_sc_t fd, nt_sc_t address, nt_sc_t length);
nt_sc_t nt_sys_connect(nt_sc_t fd, nt_sc_t address, nt_sc_t length);
nt_sc_t nt_sys_listen(nt_sc_t fd, nt_sc_t backlog);
nt_sc_t nt_sys_accept4(nt_sc_t fd, nt_sc_t address, nt_sc_t length,
                       nt_sc_t flags);
nt_sc_t nt_sys_sendto(nt_sc_t fd, nt_sc_t buffer, nt_sc_t length,
                      nt_sc_t flags, nt_sc_t address, nt_sc_t address_length);
nt_sc_t nt_sys_recvfrom(nt_sc_t fd, nt_sc_t buffer, nt_sc_t length,
                        nt_sc_t flags, nt_sc_t address, nt_sc_t address_length);
nt_sc_t nt_sys_sendmsg(nt_sc_t fd, nt_sc_t message, nt_sc_t flags);
nt_sc_t nt_sys_recvmsg(nt_sc_t fd, nt_sc_t message, nt_sc_t flags);
nt_sc_t nt_sys_recvmmsg(nt_sc_t fd, nt_sc_t messages, nt_sc_t count,
                        nt_sc_t flags, nt_sc_t timeout);
nt_sc_t nt_sys_getsockname(nt_sc_t fd, nt_sc_t address, nt_sc_t length);
nt_sc_t nt_sys_getpeername(nt_sc_t fd, nt_sc_t address, nt_sc_t length);
nt_sc_t nt_sys_shutdown(nt_sc_t fd, nt_sc_t how);
nt_sc_t nt_sys_setsockopt(nt_sc_t fd, nt_sc_t level, nt_sc_t option,
                          nt_sc_t value, nt_sc_t length);
nt_sc_t nt_sys_getsockopt(nt_sc_t fd, nt_sc_t level, nt_sc_t option,
                          nt_sc_t value, nt_sc_t length);

nt_sc_t nt_sys_exit(nt_sc_t code);
nt_sc_t nt_sys_getpid(void);
nt_sc_t nt_sys_getppid(void);
nt_sc_t nt_sys_set_tid_address(nt_sc_t ptr);
nt_sc_t nt_sys_execve(nt_sc_t path, nt_sc_t argv, nt_sc_t envp);
nt_sc_t nt_sys_spawn(nt_sc_t path, nt_sc_t argv, nt_sc_t envp);
nt_sc_t nt_sys_fork(void);
nt_sc_t nt_sys_wait4(nt_sc_t pid, nt_sc_t status, nt_sc_t options, nt_sc_t rusage);
nt_sc_t nt_sys_kill(nt_sc_t pid, nt_sc_t sig);

nt_sc_t nt_sys_clock_gettime(nt_sc_t clockid, nt_sc_t ts);
nt_sc_t nt_sys_clock_getres(nt_sc_t clockid, nt_sc_t ts);
nt_sc_t nt_sys_clock_settime(nt_sc_t clockid, nt_sc_t ts);
nt_sc_t nt_sys_clock_nanosleep(nt_sc_t clockid, nt_sc_t flags, nt_sc_t req, nt_sc_t rem);
nt_sc_t nt_sys_gettimeofday(nt_sc_t tv, nt_sc_t tz);

nt_sc_t nt_sys_ioctl(nt_sc_t fd, nt_sc_t request, nt_sc_t arg);
nt_sc_t nt_sys_rt_sigaction(nt_sc_t sig, nt_sc_t act, nt_sc_t oldact, nt_sc_t size);
nt_sc_t nt_sys_rt_sigprocmask(nt_sc_t how, nt_sc_t set, nt_sc_t oldset, nt_sc_t size);
nt_sc_t nt_sys_futex(nt_sc_t address, nt_sc_t operation, nt_sc_t value, nt_sc_t timeout,
                  nt_sc_t address2, nt_sc_t value3);

nt_sc_t nt_sys_stub(nt_sc_t number, nt_sc_t a1, nt_sc_t a2, nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                 nt_sc_t a6);

NT_SYSV_ABI nt_sc_t __nt_syscall(nt_sc_t number, nt_sc_t a1, nt_sc_t a2,
                                 nt_sc_t a3, nt_sc_t a4, nt_sc_t a5,
                                 nt_sc_t a6);

#ifdef __cplusplus
}
#endif
#endif
