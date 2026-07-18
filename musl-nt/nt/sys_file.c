#include "nt/ntpriv.h"

static enum nt_fd_kind kind_from_handle(HANDLE h)
{
    BY_HANDLE_FILE_INFORMATION info;
    DWORD type = GetFileType(h);
    if (type == FILE_TYPE_CHAR) return NT_FD_CONSOLE;
    if (type == FILE_TYPE_PIPE) return NT_FD_PIPE;
    if (type == FILE_TYPE_DISK && GetFileInformationByHandle(h, &info) &&
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        return NT_FD_DIR;
    return NT_FD_FILE;
}

static DWORD access_from_flags(int flags)
{
    if (flags & NT_O_PATH) return FILE_READ_ATTRIBUTES | SYNCHRONIZE;
    switch (flags & NT_O_ACCMODE) {
    case NT_O_WRONLY:
        return (flags & NT_O_APPEND) ? FILE_APPEND_DATA : GENERIC_WRITE;
    case NT_O_RDWR:
        return GENERIC_READ | GENERIC_WRITE;
    default:
        return GENERIC_READ;
    }
}

static DWORD disposition_from_flags(int flags)
{
    if (!(flags & NT_O_CREAT))
        return (flags & NT_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    if (flags & NT_O_EXCL) return CREATE_NEW;
    if (flags & NT_O_TRUNC) return CREATE_ALWAYS;
    return OPEN_ALWAYS;
}

static int nt_random_fill(void *buffer, size_t length)
{
    typedef BOOLEAN (WINAPI *random_fn)(PVOID, ULONG);
    static random_fn random_bytes;
    size_t done = 0;
    if (!random_bytes) {
        HMODULE advapi = LoadLibraryW(L"advapi32.dll");
        if (advapi)
            random_bytes = (random_fn)(void *)
                GetProcAddress(advapi, "SystemFunction036");
    }
    if (!random_bytes) return -1;
    while (done < length) {
        ULONG chunk = length - done > 0xffffffffU
                        ? 0xffffffffU : (ULONG)(length - done);
        if (!random_bytes((unsigned char *)buffer + done, chunk)) return -1;
        done += chunk;
    }
    return 0;
}

/* /dev mínimo: dispositivos que o Linux expõe via devtmpfs e que programas
 * abrem por caminho. Sem isto, "/dev/null" resolvia para X:\...\dev\null
 * (inexistente) -> ENOENT, quebrando `cmd 2>/dev/null`, `true & wait`, etc.
 * Retorna 1 se tratou (result = fd ou -errno); 0 se nao e um /dev conhecido
 * (ai segue a resolucao normal de path). */
static int nt_dev_open(const char *path, int flags, nt_sc_t *result)
{
    const char *rest;
    HANDLE h;
    int devkind = NT_DEV_NONE, fd;
    enum nt_fd_kind kind = NT_FD_FILE;
    if (!(path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
          path[3] == 'v' && path[4] == '/'))
        return 0;
    rest = path + 5;
    if (nt_streq(rest, "stdin"))
        { *result = nt_fd_dup(0, 0, (flags & NT_O_CLOEXEC) != 0); return 1; }
    if (nt_streq(rest, "stdout"))
        { *result = nt_fd_dup(1, 0, (flags & NT_O_CLOEXEC) != 0); return 1; }
    if (nt_streq(rest, "stderr"))
        { *result = nt_fd_dup(2, 0, (flags & NT_O_CLOEXEC) != 0); return 1; }
    if (rest[0] == 'f' && rest[1] == 'd' && rest[2] == '/' && rest[3]) {
        int n = 0; const char *d;
        for (d = rest + 3; *d; ++d) {
            if (*d < '0' || *d > '9') { n = -1; break; }
            n = n * 10 + (*d - '0');
        }
        if (n >= 0) *result = nt_fd_dup(n, 0, (flags & NT_O_CLOEXEC) != 0);
        else *result = -NT_ENOENT;
        return 1;
    }
    if (nt_streq(rest, "null")) {
        h = CreateFileW(L"NUL", access_from_flags(flags),
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        0, OPEN_EXISTING, 0, 0);
    } else if (nt_streq(rest, "zero") || nt_streq(rest, "full") ||
               nt_streq(rest, "random") || nt_streq(rest, "urandom")) {
        h = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        0, OPEN_EXISTING, 0, 0);
        devkind = nt_streq(rest, "zero") ? NT_DEV_ZERO
                : nt_streq(rest, "full") ? NT_DEV_FULL : NT_DEV_RANDOM;
    } else if (nt_streq(rest, "tty") || nt_streq(rest, "console")) {
        int wr = (flags & NT_O_ACCMODE) != NT_O_RDONLY;
        h = CreateFileW(wr ? L"CONOUT$" : L"CONIN$",
                        GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        0, OPEN_EXISTING, 0, 0);
        kind = NT_FD_CONSOLE;
    } else {
        return 0; /* /dev/<desconhecido>: cai na resolução normal (ENOENT) */
    }
    if (h == INVALID_HANDLE_VALUE) { *result = nt_last_error(); return 1; }
    fd = nt_fd_alloc(h, kind, flags, (flags & NT_O_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (fd < 0) { CloseHandle(h); *result = fd; return 1; }
    if (devkind != NT_DEV_NONE) nt_fd_set_devkind(fd, devkind);
    *result = fd;
    return 1;
}

nt_sc_t nt_sys_openat(nt_sc_t dirfd, nt_sc_t path_arg, nt_sc_t flags_arg, nt_sc_t mode)
{
    const char *path = (const char *)(uintptr_t)path_arg;
    int flags = (int)flags_arg;
    WCHAR wide[NT_PATH_CAP];
    HANDLE h;
    enum nt_fd_kind kind;
    DWORD attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS;
    int result;
    nt_sc_t r;
    (void)mode;
    if (path) {
        nt_sc_t devr;
        if (nt_dev_open(path, flags, &devr)) return devr;
    }
    r = nt_path_at((int)dirfd, path, wide, NT_ARRAY_LEN(wide));
    if (r < 0) return r;
    if (flags & NT_O_NOFOLLOW) attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
    h = CreateFileW(wide, access_from_flags(flags),
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    0, disposition_from_flags(flags), attrs, 0);
    if (h == INVALID_HANDLE_VALUE) return nt_last_error();
    kind = kind_from_handle(h);
    if ((flags & NT_O_DIRECTORY) && kind != NT_FD_DIR) {
        CloseHandle(h);
        return -NT_ENOTDIR;
    }
    result = nt_fd_alloc(h, kind, flags,
                         (flags & NT_O_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (result < 0) CloseHandle(h);
    return result;
}

static nt_sc_t transfer_read(struct nt_fd *slot, void *buf, uint64_t count)
{
    DWORD got;
    DWORD amount;
    if (slot->kind == NT_FD_DIR) return -NT_EISDIR;
    if (!buf && count) return -NT_EFAULT;
    amount = count > 0x7ffff000ULL ? 0x7ffff000UL : (DWORD)count;
    if (slot->devkind == NT_DEV_ZERO || slot->devkind == NT_DEV_FULL) {
        if (amount) nt_memset(buf, 0, amount); /* /dev/zero e /dev/full leem zeros */
        return amount;
    }
    if (slot->devkind == NT_DEV_RANDOM) {
        if (amount && nt_random_fill(buf, amount) < 0) return -NT_EIO;
        return amount;
    }
    if (slot->kind == NT_FD_SOCKET)
        return nt_socket_read(slot, buf, amount);
    /* Pipe não-bloqueante: CreatePipe só oferece I/O bloqueante, então emulamos
     * o O_NONBLOCK da leitura espiando antes. Sem dados e sem EOF => EAGAIN,
     * como no Linux; senão o ReadFile abaixo retorna o que já há sem bloquear. */
    if ((slot->kind == NT_FD_PIPE || slot->kind == NT_FD_PTY) &&
        (slot->flags & NT_O_NONBLOCK)) {
        DWORD avail = 0;
        if (PeekNamedPipe(slot->handle, 0, 0, 0, &avail, 0)) {
            if (!avail) return -NT_EAGAIN;
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            return 0;
        }
    }
    if (!ReadFile(slot->handle, buf, amount, &got, 0)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
        /* CancelSynchronousIo (Ctrl-C acordando um read bloqueado) -> EINTR;
         * o sinal pendente é entregue no retorno da syscall. */
        if (e == ERROR_OPERATION_ABORTED) return -NT_EINTR;
        return nt_error(e);
    }
    /* console em modo cooked entrega linhas com \r\n; um terminal Unix
     * entrega só \n. Remove os \r (equivalente ao ICRNL). Em raw mode
     * (line input desligado) o programa quer os bytes crus, então não mexe. */
    if (slot->kind == NT_FD_CONSOLE && got > 0) {
        DWORD mode;
        if (GetConsoleMode(slot->handle, &mode) && (mode & ENABLE_LINE_INPUT)) {
            unsigned char *b = buf;
            DWORD i, j = 0;
            for (i = 0; i < got; ++i)
                if (b[i] != '\r') b[j++] = b[i];
            got = j;
        }
    }
    return got;
}

static nt_sc_t transfer_write(struct nt_fd *slot, const void *buf, uint64_t count)
{
    DWORD put;
    DWORD amount;
    LARGE_INTEGER end;
    if (slot->kind == NT_FD_DIR) return -NT_EISDIR;
    if (!buf && count) return -NT_EFAULT;
    amount = count > 0x7ffff000ULL ? 0x7ffff000UL : (DWORD)count;
    if (slot->devkind == NT_DEV_FULL) return -NT_ENOSPC; /* /dev/full */
    if (slot->devkind != NT_DEV_NONE) return amount; /* /dev/zero,/dev/random: descarta */
    if (slot->kind == NT_FD_SOCKET)
        return nt_socket_write(slot, buf, amount);
    AcquireSRWLockExclusive(&slot->io_lock);
    if ((slot->flags & NT_O_APPEND) && slot->kind == NT_FD_FILE) {
        end.QuadPart = 0;
        if (!SetFilePointerEx(slot->handle, end, 0, FILE_END)) {
            nt_sc_t r = nt_last_error();
            ReleaseSRWLockExclusive(&slot->io_lock);
            return r;
        }
    }
    if (slot->kind == NT_FD_PTY && nt_pty_onlcr()) {
        /* disciplina de saida do tty: \n -> \r\n (senao o VT escadeia o texto) */
        const unsigned char *b = buf;
        DWORD i, start = 0, w;
        for (i = 0; i < amount; i++) {
            if (b[i] != '\n') continue;
            if (i > start && !WriteFile(slot->handle, b + start, i - start, &w, 0)) {
                nt_sc_t r = nt_last_error();
                ReleaseSRWLockExclusive(&slot->io_lock);
                return r;
            }
            if (!WriteFile(slot->handle, "\r\n", 2, &w, 0)) {
                nt_sc_t r = nt_last_error();
                ReleaseSRWLockExclusive(&slot->io_lock);
                return r;
            }
            start = i + 1;
        }
        if (amount > start && !WriteFile(slot->handle, b + start, amount - start, &w, 0)) {
            nt_sc_t r = nt_last_error();
            ReleaseSRWLockExclusive(&slot->io_lock);
            return r;
        }
        ReleaseSRWLockExclusive(&slot->io_lock);
        return amount;   /* conta os bytes logicos, sem os \r injetados */
    }
    if (!WriteFile(slot->handle, buf, amount, &put, 0)) {
        nt_sc_t r = nt_last_error();
        ReleaseSRWLockExclusive(&slot->io_lock);
        return r;
    }
    ReleaseSRWLockExclusive(&slot->io_lock);
    return put;
}

nt_sc_t nt_sys_read(nt_sc_t fd, nt_sc_t buf, nt_sc_t count)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    if (!slot) return -NT_EBADF;
    if (count < 0) return -NT_EINVAL;
    return transfer_read(slot, (void *)(uintptr_t)buf, (uint64_t)count);
}

nt_sc_t nt_sys_write(nt_sc_t fd, nt_sc_t buf, nt_sc_t count)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    if (!slot) return -NT_EBADF;
    if (count < 0) return -NT_EINVAL;
    return transfer_write(slot, (const void *)(uintptr_t)buf, (uint64_t)count);
}

nt_sc_t nt_sys_readv(nt_sc_t fd, nt_sc_t iov_arg, nt_sc_t count)
{
    const struct nt_iovec *iov = (const void *)(uintptr_t)iov_arg;
    nt_sc_t total = 0, i;
    if (!iov && count) return -NT_EFAULT;
    if (count < 0 || count > 1024) return -NT_EINVAL;
    for (i = 0; i < count; ++i) {
        nt_sc_t r = nt_sys_read(fd, (nt_sc_t)(uintptr_t)iov[i].iov_base,
                             (nt_sc_t)iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
    }
    return total;
}

nt_sc_t nt_sys_writev(nt_sc_t fd, nt_sc_t iov_arg, nt_sc_t count)
{
    const struct nt_iovec *iov = (const void *)(uintptr_t)iov_arg;
    nt_sc_t total = 0, i;
    if (!iov && count) return -NT_EFAULT;
    if (count < 0 || count > 1024) return -NT_EINVAL;
    for (i = 0; i < count; ++i) {
        nt_sc_t r = nt_sys_write(fd, (nt_sc_t)(uintptr_t)iov[i].iov_base,
                              (nt_sc_t)iov[i].iov_len);
        if (r < 0) return total ? total : r;
        total += r;
        if ((uint64_t)r < iov[i].iov_len) break;
    }
    return total;
}

nt_sc_t nt_sys_close(nt_sc_t fd)
{
    return nt_fd_close((int)fd);
}

nt_sc_t nt_sys_lseek(nt_sc_t fd, nt_sc_t offset, nt_sc_t whence)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    LARGE_INTEGER in, out;
    DWORD method;
    if (!slot) return -NT_EBADF;
    if (slot->kind == NT_FD_PIPE || slot->kind == NT_FD_CONSOLE ||
        slot->kind == NT_FD_PTY)
        return -NT_ESPIPE;
    switch (whence) {
    case NT_SEEK_SET: method = FILE_BEGIN; break;
    case NT_SEEK_CUR: method = FILE_CURRENT; break;
    case NT_SEEK_END: method = FILE_END; break;
    default: return -NT_EINVAL;
    }
    in.QuadPart = offset;
    AcquireSRWLockExclusive(&slot->io_lock);
    if (!SetFilePointerEx(slot->handle, in, &out, method)) {
        nt_sc_t r = nt_last_error();
        ReleaseSRWLockExclusive(&slot->io_lock);
        return r;
    }
    ReleaseSRWLockExclusive(&slot->io_lock);
    return (nt_sc_t)out.QuadPart;
}

nt_sc_t nt_sys_dup(nt_sc_t fd)
{
    return nt_fd_dup((int)fd, 0, 0);
}

nt_sc_t nt_sys_dup2(nt_sc_t oldfd, nt_sc_t newfd)
{
    return nt_fd_dup2((int)oldfd, (int)newfd, 0);
}

nt_sc_t nt_sys_dup3(nt_sc_t oldfd, nt_sc_t newfd, nt_sc_t flags)
{
    if (oldfd == newfd) return -NT_EINVAL;
    if (flags & ~NT_O_CLOEXEC) return -NT_EINVAL;
    return nt_fd_dup2((int)oldfd, (int)newfd,
                      (flags & NT_O_CLOEXEC) != 0);
}

nt_sc_t nt_sys_fcntl(nt_sc_t fd, nt_sc_t cmd, nt_sc_t arg)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    if (!slot) return -NT_EBADF;
    switch (cmd) {
    case NT_F_DUPFD: return nt_fd_dup((int)fd, (int)arg, 0);
    case NT_F_DUPFD_CLOEXEC: return nt_fd_dup((int)fd, (int)arg, 1);
    case NT_F_GETFD: return slot->fd_flags;
    case NT_F_SETFD:
        slot->fd_flags = (int)arg & NT_FD_CLOEXEC;
        return 0;
    case NT_F_GETFL: return slot->flags;
    case NT_F_SETFL:
        slot->flags = (slot->flags & ~(NT_O_APPEND | NT_O_NONBLOCK)) |
                      ((int)arg & (NT_O_APPEND | NT_O_NONBLOCK));
        if (slot->kind == NT_FD_SOCKET)
            return nt_socket_set_nonblocking(
                slot, (slot->flags & NT_O_NONBLOCK) != 0);
        return 0;
    default:
        return -NT_EINVAL;
    }
}

nt_sc_t nt_sys_ftruncate(nt_sc_t fd, nt_sc_t length)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    LARGE_INTEGER zero, saved, target;
    nt_sc_t result = 0;
    if (!slot) return -NT_EBADF;
    if (length < 0) return -NT_EINVAL;
    if (slot->kind != NT_FD_FILE) return -NT_EINVAL;
    zero.QuadPart = 0;
    target.QuadPart = length;
    AcquireSRWLockExclusive(&slot->io_lock);
    if (!SetFilePointerEx(slot->handle, zero, &saved, FILE_CURRENT) ||
        !SetFilePointerEx(slot->handle, target, 0, FILE_BEGIN) ||
        !SetEndOfFile(slot->handle))
        result = nt_last_error();
    SetFilePointerEx(slot->handle, saved, 0, FILE_BEGIN);
    ReleaseSRWLockExclusive(&slot->io_lock);
    return result;
}

nt_sc_t nt_sys_truncate(nt_sc_t path, nt_sc_t length)
{
    nt_sc_t fd = nt_sys_openat(NT_AT_FDCWD, path, NT_O_WRONLY, 0);
    nt_sc_t r;
    if (fd < 0) return fd;
    r = nt_sys_ftruncate(fd, length);
    nt_sys_close(fd);
    return r;
}

static nt_sc_t positional_io(nt_sc_t fd, void *buf, uint64_t count, int64_t offset,
                          int write_operation)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    LARGE_INTEGER zero, saved, target;
    DWORD done = 0, amount;
    nt_sc_t result;
    if (!slot) return -NT_EBADF;
    if (slot->kind != NT_FD_FILE) return -NT_ESPIPE;
    if (offset < 0) return -NT_EINVAL;
    amount = count > 0x7ffff000ULL ? 0x7ffff000UL : (DWORD)count;
    zero.QuadPart = 0;
    target.QuadPart = offset;
    AcquireSRWLockExclusive(&slot->io_lock);
    if (!SetFilePointerEx(slot->handle, zero, &saved, FILE_CURRENT) ||
        !SetFilePointerEx(slot->handle, target, 0, FILE_BEGIN)) {
        result = nt_last_error();
    } else if (write_operation
               ? !WriteFile(slot->handle, buf, amount, &done, 0)
               : !ReadFile(slot->handle, buf, amount, &done, 0)) {
        DWORD e = GetLastError();
        result = (!write_operation && e == ERROR_HANDLE_EOF) ? 0 : nt_error(e);
    } else {
        result = done;
    }
    SetFilePointerEx(slot->handle, saved, 0, FILE_BEGIN);
    ReleaseSRWLockExclusive(&slot->io_lock);
    return result;
}

nt_sc_t nt_sys_pread(nt_sc_t fd, nt_sc_t buf, nt_sc_t count, nt_sc_t offset)
{
    if (count < 0 || (!buf && count)) return -NT_EFAULT;
    return positional_io(fd, (void *)(uintptr_t)buf, (uint64_t)count, offset, 0);
}

nt_sc_t nt_sys_pwrite(nt_sc_t fd, nt_sc_t buf, nt_sc_t count, nt_sc_t offset)
{
    if (count < 0 || (!buf && count)) return -NT_EFAULT;
    return positional_io(fd, (void *)(uintptr_t)buf, (uint64_t)count, offset, 1);
}

nt_sc_t nt_sys_pipe2(nt_sc_t pipefd_arg, nt_sc_t flags)
{
    int *pipefd = (int *)(uintptr_t)pipefd_arg;
    HANDLE read_handle, write_handle;
    int read_fd, write_fd;
    if (!pipefd) return -NT_EFAULT;
    if (flags & ~(NT_O_CLOEXEC | NT_O_NONBLOCK)) return -NT_EINVAL;
    {
        SECURITY_ATTRIBUTES sa;
        sa.nLength = sizeof sa;
        sa.lpSecurityDescriptor = 0;
        sa.bInheritHandle = TRUE; /* herdável: precisa atravessar o fork */
        if (!CreatePipe(&read_handle, &write_handle, &sa, 0)) return nt_last_error();
    }
    if (flags & NT_O_CLOEXEC) {
        SetHandleInformation(read_handle, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(write_handle, HANDLE_FLAG_INHERIT, 0);
    }
    read_fd = nt_fd_alloc(read_handle, NT_FD_PIPE, NT_O_RDONLY | (int)flags,
                          (flags & NT_O_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (read_fd < 0) {
        CloseHandle(read_handle); CloseHandle(write_handle);
        return read_fd;
    }
    write_fd = nt_fd_alloc(write_handle, NT_FD_PIPE, NT_O_WRONLY | (int)flags,
                           (flags & NT_O_CLOEXEC) ? NT_FD_CLOEXEC : 0, 0);
    if (write_fd < 0) {
        nt_fd_close(read_fd); CloseHandle(write_handle);
        return write_fd;
    }
    pipefd[0] = read_fd;
    pipefd[1] = write_fd;
    return 0;
}

static int poll_one(struct nt_pollfd *p)
{
    struct nt_fd *slot = nt_fd_get(p->fd);
    DWORD available = 0;
    p->revents = 0;
    if (!slot) {
        p->revents = NT_POLLNVAL;
        return 1;
    }
    if (slot->kind == NT_FD_SOCKET)
        return nt_socket_poll(slot, p->events, &p->revents);
    if (p->events & NT_POLLOUT) p->revents |= NT_POLLOUT;
    if (p->events & NT_POLLIN) {
        if (slot->kind == NT_FD_PIPE) {
            if (PeekNamedPipe(slot->handle, 0, 0, 0, &available, 0)) {
                if (available) p->revents |= NT_POLLIN;
            } else if (GetLastError() == ERROR_BROKEN_PIPE) {
                p->revents |= NT_POLLHUP;
            }
        } else if (slot->kind == NT_FD_CONSOLE) {
            if (GetNumberOfConsoleInputEvents(slot->handle, &available) && available)
                p->revents |= NT_POLLIN;
        } else {
            p->revents |= NT_POLLIN;
        }
    }
    return p->revents != 0;
}

nt_sc_t nt_sys_poll(nt_sc_t fds_arg, nt_sc_t nfds, nt_sc_t timeout)
{
    struct nt_pollfd *fds = (void *)(uintptr_t)fds_arg;
    ULONGLONG start = GetTickCount64();
    if ((!fds && nfds) || nfds < 0 || nfds > 65536) return -NT_EINVAL;
    for (;;) {
        nt_sc_t i, ready = 0;
        for (i = 0; i < nfds; ++i) ready += poll_one(&fds[i]);
        if (ready || timeout == 0) return ready;
        if (timeout > 0 && GetTickCount64() - start >= (ULONGLONG)timeout)
            return 0;
        Sleep(1);
    }
}
