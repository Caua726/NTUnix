#include "nt/ntpriv.h"

static struct nt_fd fd_table[NT_MAX_FDS];
static SRWLOCK table_lock = SRWLOCK_INIT;
static INIT_ONCE table_once = INIT_ONCE_STATIC_INIT;

static int close_native(HANDLE handle, enum nt_fd_kind kind)
{
    if (kind == NT_FD_SOCKET)
        return nt_socket_close(handle);
    return CloseHandle(handle) != 0;
}

static enum nt_fd_kind kind_for_handle(HANDLE h)
{
    DWORD type;
    if (!h || h == INVALID_HANDLE_VALUE) return NT_FD_NONE;
    type = GetFileType(h);
    if (type == FILE_TYPE_CHAR) return NT_FD_CONSOLE;
    if (type == FILE_TYPE_PIPE) return NT_FD_PIPE;
    return NT_FD_FILE;
}

static BOOL CALLBACK initialize_table(PINIT_ONCE once, PVOID parameter,
                                      PVOID *context)
{
    int i;
    (void)once;
    (void)parameter;
    (void)context;
    for (i = 0; i < NT_MAX_FDS; ++i)
        InitializeSRWLock(&fd_table[i].io_lock);

    fd_table[0].handle = GetStdHandle(STD_INPUT_HANDLE);
    fd_table[1].handle = GetStdHandle(STD_OUTPUT_HANDLE);
    fd_table[2].handle = GetStdHandle(STD_ERROR_HANDLE);
    for (i = 0; i < 3; ++i) {
        fd_table[i].kind = kind_for_handle(fd_table[i].handle);
        fd_table[i].flags = i ? NT_O_WRONLY : NT_O_RDONLY;
    }
    /* pty nativo: quando o pai (dispd) marca NTU_PTY, os std handles sao pipes
     * do master do pty — tratamos como tty (isatty/termios em memoria), nao
     * como pipe cru. Assim o ash entra em modo interativo sem ConPTY. */
    {
        char v[8];
        if (GetEnvironmentVariableA("NTU_PTY", v, sizeof v) > 0)
            for (i = 0; i < 3; ++i)
                if (fd_table[i].kind == NT_FD_PIPE)
                    fd_table[i].kind = NT_FD_PTY;
    }
    return TRUE;
}

void nt_fdtable_init(void)
{
    InitOnceExecuteOnce(&table_once, initialize_table, 0, 0);
}

struct nt_fd *nt_fd_get(int fd)
{
    struct nt_fd *result = 0;
    nt_fdtable_init();
    if (fd < 0 || fd >= NT_MAX_FDS) return 0;
    AcquireSRWLockShared(&table_lock);
    if (fd_table[fd].kind != NT_FD_NONE && fd_table[fd].handle &&
        fd_table[fd].handle != INVALID_HANDLE_VALUE)
        result = &fd_table[fd];
    ReleaseSRWLockShared(&table_lock);
    return result;
}

HANDLE nt_fd_handle(int fd)
{
    struct nt_fd *slot = nt_fd_get(fd);
    return slot ? slot->handle : INVALID_HANDLE_VALUE;
}

int nt_fd_alloc(HANDLE h, enum nt_fd_kind kind, int flags, int fd_flags,
                int minimum)
{
    int i;
    if (!h || h == INVALID_HANDLE_VALUE) return -NT_EBADF;
    if (minimum < 0) minimum = 0;
    if (minimum >= NT_MAX_FDS) return -NT_EINVAL;
    nt_fdtable_init();
    AcquireSRWLockExclusive(&table_lock);
    for (i = minimum; i < NT_MAX_FDS; ++i) {
        if (fd_table[i].kind == NT_FD_NONE) {
            fd_table[i].handle = h;
            fd_table[i].kind = kind;
            fd_table[i].flags = flags;
            fd_table[i].fd_flags = fd_flags;
            fd_table[i].devkind = NT_DEV_NONE;
            fd_table[i].dir_eof = 0;
            fd_table[i].dir_cookie = 0;
            ReleaseSRWLockExclusive(&table_lock);
            return i;
        }
    }
    ReleaseSRWLockExclusive(&table_lock);
    return -NT_EMFILE;
}

nt_sc_t nt_fd_install_at(int fd, HANDLE h, enum nt_fd_kind kind, int flags,
                      int fd_flags)
{
    HANDLE old = INVALID_HANDLE_VALUE;
    enum nt_fd_kind old_kind = NT_FD_NONE;
    if (fd < 0 || fd >= NT_MAX_FDS || !h || h == INVALID_HANDLE_VALUE)
        return -NT_EBADF;
    nt_fdtable_init();
    AcquireSRWLockExclusive(&table_lock);
    if (fd_table[fd].kind != NT_FD_NONE) {
        old = fd_table[fd].handle;
        old_kind = fd_table[fd].kind;
    }
    fd_table[fd].handle = h;
    fd_table[fd].kind = kind;
    fd_table[fd].flags = flags;
    fd_table[fd].fd_flags = fd_flags;
    fd_table[fd].devkind = NT_DEV_NONE;
    fd_table[fd].dir_eof = 0;
    fd_table[fd].dir_cookie = 0;
    ReleaseSRWLockExclusive(&table_lock);
    if (old && old != INVALID_HANDLE_VALUE) close_native(old, old_kind);
    return fd;
}

nt_sc_t nt_fd_close(int fd)
{
    HANDLE h;
    enum nt_fd_kind kind;
    if (fd < 0 || fd >= NT_MAX_FDS) return -NT_EBADF;
    nt_fdtable_init();
    AcquireSRWLockExclusive(&table_lock);
    if (fd_table[fd].kind == NT_FD_NONE) {
        ReleaseSRWLockExclusive(&table_lock);
        return -NT_EBADF;
    }
    h = fd_table[fd].handle;
    kind = fd_table[fd].kind;
    fd_table[fd].handle = INVALID_HANDLE_VALUE;
    fd_table[fd].kind = NT_FD_NONE;
    fd_table[fd].flags = 0;
    fd_table[fd].fd_flags = 0;
    fd_table[fd].dir_eof = 0;
    fd_table[fd].dir_cookie = 0;
    ReleaseSRWLockExclusive(&table_lock);
    if (!close_native(h, kind))
        return kind == NT_FD_SOCKET ? nt_wsa_error() : nt_last_error();
    return 0;
}

nt_sc_t nt_fd_dup(int oldfd, int minimum, int cloexec)
{
    struct nt_fd *old = nt_fd_get(oldfd);
    HANDLE copy;
    int result, devkind;
    nt_sc_t r;
    if (!old) return -NT_EBADF;
    devkind = old->devkind;
    if (old->kind == NT_FD_SOCKET) {
        r = nt_socket_duplicate(old->handle, cloexec, &copy);
        if (r < 0) return r;
    } else if (!DuplicateHandle(GetCurrentProcess(), old->handle,
                                GetCurrentProcess(), &copy, 0, !cloexec,
                                DUPLICATE_SAME_ACCESS)) {
        return nt_last_error();
    }
    result = nt_fd_alloc(copy, old->kind, old->flags,
                         cloexec ? NT_FD_CLOEXEC : 0, minimum);
    if (result < 0) close_native(copy, old->kind);
    else if (devkind != NT_DEV_NONE) nt_fd_set_devkind(result, devkind);
    return result;
}

void nt_fd_make_inheritable(void)
{
    /* Antes de um fork (RtlCloneUserProcess com INHERIT_HANDLES), marca todos
     * os handles da tabela como herdáveis — senão o filho clonado recebe
     * apenas a cópia da memória, mas os handles (pipes, arquivos) ficam
     * inválidos nele. É o equivalente a "todos os fds atravessam o fork". */
    int i;
    nt_fdtable_init();
    AcquireSRWLockShared(&table_lock);
    for (i = 0; i < NT_MAX_FDS; ++i) {
        struct nt_fd *s = &fd_table[i];
        if (s->kind != NT_FD_NONE && s->kind != NT_FD_SOCKET &&
            s->handle && s->handle != INVALID_HANDLE_VALUE)
            SetHandleInformation(s->handle, HANDLE_FLAG_INHERIT,
                                 HANDLE_FLAG_INHERIT);
    }
    ReleaseSRWLockShared(&table_lock);
}

void nt_fd_reattach_console(void)
{
    /* Após um fork (RtlCloneUserProcess), o processo se reanexa ao console do
     * pai, mas os handles de console herdados ficam órfãos (o clone não
     * clona a conexão com o conhost). Reabre CONIN$/CONOUT$ e reassocia
     * fd 0/1/2 quando ainda forem console (o dup2 de um pipeline vem depois,
     * então aqui eles ainda são o console). */
    HANDLE in, out;
    nt_fdtable_init();
    in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                     FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                      FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    AcquireSRWLockExclusive(&table_lock);
    if (in != INVALID_HANDLE_VALUE && fd_table[0].kind == NT_FD_CONSOLE) {
        fd_table[0].handle = in;
        SetStdHandle(STD_INPUT_HANDLE, in);
    }
    if (out != INVALID_HANDLE_VALUE && fd_table[1].kind == NT_FD_CONSOLE) {
        fd_table[1].handle = out;
        SetStdHandle(STD_OUTPUT_HANDLE, out);
    }
    if (out != INVALID_HANDLE_VALUE && fd_table[2].kind == NT_FD_CONSOLE) {
        HANDLE err2;
        if (DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(),
                            &err2, 0, TRUE, DUPLICATE_SAME_ACCESS)) {
            fd_table[2].handle = err2;
            SetStdHandle(STD_ERROR_HANDLE, err2);
        }
    }
    ReleaseSRWLockExclusive(&table_lock);
}

nt_sc_t nt_fd_dup2(int oldfd, int newfd, int cloexec)
{
    struct nt_fd *old;
    HANDLE copy;
    nt_sc_t r;
    int devkind;
    if (oldfd == newfd)
        return nt_fd_get(oldfd) ? oldfd : -NT_EBADF;
    if (newfd < 0 || newfd >= NT_MAX_FDS) return -NT_EBADF;
    old = nt_fd_get(oldfd);
    if (!old) return -NT_EBADF;
    devkind = old->devkind;
    if (old->kind == NT_FD_SOCKET) {
        r = nt_socket_duplicate(old->handle, cloexec, &copy);
        if (r < 0) return r;
    } else if (!DuplicateHandle(GetCurrentProcess(), old->handle,
                                GetCurrentProcess(), &copy, 0, !cloexec,
                                DUPLICATE_SAME_ACCESS)) {
        return nt_last_error();
    }
    r = nt_fd_install_at(newfd, copy, old->kind, old->flags,
                         cloexec ? NT_FD_CLOEXEC : 0);
    if (r >= 0 && devkind != NT_DEV_NONE) nt_fd_set_devkind(newfd, devkind);
    return r;
}

void nt_fd_set_devkind(int fd, int devkind)
{
    if (fd < 0 || fd >= NT_MAX_FDS) return;
    nt_fdtable_init();
    AcquireSRWLockExclusive(&table_lock);
    fd_table[fd].devkind = devkind;
    ReleaseSRWLockExclusive(&table_lock);
}
