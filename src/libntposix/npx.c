/*
 * npx.c — libntposix v0, backend Win32.
 *
 * Cada grupo de funcoes tem um "TODO Native API" apontando a chamada ntdll
 * que vai substituir o Win32 numa fase posterior (docs/pesquisa/
 * nt-native-api.md). A troca e local: a interface em npx.h nao muda.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include <ctype.h>
#include "npx.h"
#include "../common/ntu.h"

static DWORD g_err;

/* ------------- tabela de descritores (pesquisa §7) ------------- */
#define NPX_MAXFD 256
typedef struct { HANDLE h; int used; int append; } FdSlot;
static FdSlot g_fd[NPX_MAXFD];
static int g_fd_init;

static void fd_init(void)
{
    if (g_fd_init) return;
    g_fd_init = 1;
    g_fd[0] = (FdSlot){ GetStdHandle(STD_INPUT_HANDLE),  1, 0 };
    g_fd[1] = (FdSlot){ GetStdHandle(STD_OUTPUT_HANDLE), 1, 0 };
    g_fd[2] = (FdSlot){ GetStdHandle(STD_ERROR_HANDLE),  1, 0 };
}

static int fd_alloc(HANDLE h, int append)
{
    fd_init();
    for (int i = 3; i < NPX_MAXFD; i++)
        if (!g_fd[i].used) {
            g_fd[i] = (FdSlot){ h, 1, append };
            return i;
        }
    g_err = ERROR_TOO_MANY_OPEN_FILES;
    return -1;
}

static HANDLE fd_get(int fd)
{
    fd_init();
    if (fd < 0 || fd >= NPX_MAXFD || !g_fd[fd].used)
        return INVALID_HANDLE_VALUE;
    return g_fd[fd].h;
}

/* ------------- arquivos: TODO Native API = NtCreateFile/NtReadFile ------- */
int npx_open(const char *path, int flags)
{
    DWORD access = 0, disp, share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    switch (flags & 0x3) {
    case NPX_O_RDONLY: access = GENERIC_READ; break;
    case NPX_O_WRONLY: access = GENERIC_WRITE; break;
    case NPX_O_RDWR:   access = GENERIC_READ | GENERIC_WRITE; break;
    }
    if (flags & NPX_O_APPEND) access |= FILE_APPEND_DATA;

    if (flags & NPX_O_CREAT)
        disp = (flags & NPX_O_EXCL)  ? CREATE_NEW
             : (flags & NPX_O_TRUNC) ? CREATE_ALWAYS
                                     : OPEN_ALWAYS;
    else
        disp = (flags & NPX_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;

    char win[MAX_PATH];
    ntu_path(path, win, sizeof win);
    HANDLE h = CreateFileA(win, access, share, NULL, disp,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) { g_err = GetLastError(); return -1; }
    return fd_alloc(h, (flags & NPX_O_APPEND) != 0);
}

npx_ssize npx_read(int fd, void *buf, size_t n)
{
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) { g_err = ERROR_INVALID_HANDLE; return -1; }
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)n, &got, NULL)) {
        /* fim de pipe de leitura = EOF, nao erro */
        if (GetLastError() == ERROR_BROKEN_PIPE) return 0;
        g_err = GetLastError();
        return -1;
    }
    return (npx_ssize)got;
}

npx_ssize npx_write(int fd, const void *buf, size_t n)
{
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) { g_err = ERROR_INVALID_HANDLE; return -1; }
    DWORD put = 0;
    if (!WriteFile(h, buf, (DWORD)n, &put, NULL)) { g_err = GetLastError(); return -1; }
    return (npx_ssize)put;
}

npx_off npx_lseek(int fd, npx_off off, int whence)
{
    HANDLE h = fd_get(fd);
    if (h == INVALID_HANDLE_VALUE) { g_err = ERROR_INVALID_HANDLE; return -1; }
    DWORD m = whence == NPX_SEEK_CUR ? FILE_CURRENT
            : whence == NPX_SEEK_END ? FILE_END : FILE_BEGIN;
    LARGE_INTEGER li; li.QuadPart = off;
    if (!SetFilePointerEx(h, li, &li, m)) { g_err = GetLastError(); return -1; }
    return li.QuadPart;
}

int npx_close(int fd)
{
    fd_init();
    if (fd < 3 || fd >= NPX_MAXFD || !g_fd[fd].used) {
        g_err = ERROR_INVALID_HANDLE; return -1;
    }
    CloseHandle(g_fd[fd].h);
    g_fd[fd].used = 0;
    return 0;
}

int npx_dup2(int oldfd, int newfd)
{
    HANDLE h = fd_get(oldfd);
    if (h == INVALID_HANDLE_VALUE || newfd < 0 || newfd >= NPX_MAXFD) {
        g_err = ERROR_INVALID_HANDLE; return -1;
    }
    HANDLE dup;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup,
                         0, TRUE, DUPLICATE_SAME_ACCESS)) {
        g_err = GetLastError(); return -1;
    }
    if (newfd >= 3 && g_fd[newfd].used) CloseHandle(g_fd[newfd].h);
    g_fd[newfd] = (FdSlot){ dup, 1, 0 };
    return newfd;
}

/* ------------- diretorios: TODO Native API = NtQueryDirectoryFile -------- */
struct NpxDir { HANDLE h; WIN32_FIND_DATAA fd; int first; };

NpxDir *npx_opendir(const char *path)
{
    char win[MAX_PATH], pat[MAX_PATH];
    ntu_path(path, win, sizeof win);
    snprintf(pat, sizeof pat, "%s\\*", win);
    NpxDir *d = calloc(1, sizeof *d);
    if (!d) { g_err = ERROR_NOT_ENOUGH_MEMORY; return NULL; }
    d->h = FindFirstFileA(pat, &d->fd);
    if (d->h == INVALID_HANDLE_VALUE) { g_err = GetLastError(); free(d); return NULL; }
    d->first = 1;
    return d;
}

int npx_readdir(NpxDir *d, npx_dirent *out)
{
    if (!d) return -1;
    if (!d->first) {
        if (!FindNextFileA(d->h, &d->fd)) {
            if (GetLastError() == ERROR_NO_MORE_FILES) return 0;
            g_err = GetLastError(); return -1;
        }
    }
    d->first = 0;
    strncpy(out->name, d->fd.cFileName, sizeof out->name - 1);
    out->name[sizeof out->name - 1] = 0;
    out->is_dir = (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    out->size = ((unsigned long long)d->fd.nFileSizeHigh << 32) | d->fd.nFileSizeLow;
    return 1;
}

void npx_closedir(NpxDir *d)
{
    if (!d) return;
    if (d->h != INVALID_HANDLE_VALUE) FindClose(d->h);
    free(d);
}

/* ------------- processos: TODO Native API = NtCreateUserProcess ---------- */
#define NPX_MAXPROC 64
static struct { DWORD pid; HANDLE h; } g_proc[NPX_MAXPROC];

/* resolve argv0 -> caminho Win32 executavel */
static void resolve_prog(const char *a0, char *out, size_t cap)
{
    int has_sep = strchr(a0, '/') || strchr(a0, '\\') || strchr(a0, ':');
    char cand[MAX_PATH];
    if (has_sep) {
        ntu_path(a0, out, cap);
    } else {
        /* procura em /system/bin/<a0>[.exe] */
        snprintf(cand, sizeof cand, "/system/bin/%s", a0);
        char win[MAX_PATH];
        ntu_path(cand, win, sizeof win);
        char withexe[MAX_PATH];
        snprintf(withexe, sizeof withexe, "%s.exe", win);
        if (GetFileAttributesA(win) != INVALID_FILE_ATTRIBUTES)
            strncpy(out, win, cap - 1), out[cap-1]=0;
        else if (GetFileAttributesA(withexe) != INVALID_FILE_ATTRIBUTES)
            strncpy(out, withexe, cap - 1), out[cap-1]=0;
        else /* deixa o loader do Windows resolver via PATH */
            strncpy(out, a0, cap - 1), out[cap-1]=0;
    }
}

static void build_cmdline(char *const argv[], const char *prog, char *out, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out+n, cap>n?cap-n:0, "\"%s\"", prog);
    for (int i = 1; argv[i]; i++) {
        int q = strchr(argv[i], ' ') != NULL;
        n += (size_t)snprintf(out+n, cap>n?cap-n:0, q ? " \"%s\"" : " %s", argv[i]);
    }
}

long npx_spawn(char *const argv[], int in_fd, int out_fd, int err_fd)
{
    if (!argv || !argv[0]) { g_err = ERROR_INVALID_PARAMETER; return -1; }
    char prog[MAX_PATH];
    resolve_prog(argv[0], prog, sizeof prog);
    char cmd[2048];
    build_cmdline(argv, prog, cmd, sizeof cmd);

    STARTUPINFOA si; ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    HANDLE hin  = in_fd  >= 0 ? fd_get(in_fd)  : GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hout = out_fd >= 0 ? fd_get(out_fd) : GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE herr = err_fd >= 0 ? fd_get(err_fd) : GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = hin; si.hStdOutput = hout; si.hStdError = herr;
    /* handles redirecionados precisam ser herdaveis */
    SetHandleInformation(hin,  HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(hout, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
    SetHandleInformation(herr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    PROCESS_INFORMATION pi;
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        g_err = GetLastError(); return -1;
    }
    CloseHandle(pi.hThread);
    for (int i = 0; i < NPX_MAXPROC; i++)
        if (!g_proc[i].h) { g_proc[i].pid = pi.dwProcessId; g_proc[i].h = pi.hProcess; break; }
    return (long)pi.dwProcessId;
}

int npx_waitpid(long pid, int *exit_code)
{
    for (int i = 0; i < NPX_MAXPROC; i++)
        if (g_proc[i].h && g_proc[i].pid == (DWORD)pid) {
            WaitForSingleObject(g_proc[i].h, INFINITE);
            DWORD code = 0; GetExitCodeProcess(g_proc[i].h, &code);
            if (exit_code) *exit_code = (int)code;
            CloseHandle(g_proc[i].h);
            g_proc[i].h = NULL;
            return 0;
        }
    g_err = ERROR_INVALID_PARAMETER;
    return -1;
}

/* ------------- ambiente ------------- */
int npx_chdir(const char *path)
{
    char win[MAX_PATH];
    ntu_path(path, win, sizeof win);
    if (!SetCurrentDirectoryA(win)) { g_err = GetLastError(); return -1; }
    return 0;
}

char *npx_getcwd(char *buf, size_t cap)
{
    char win[MAX_PATH];
    GetCurrentDirectoryA(sizeof win, win);
    const char *root = ntu_root();
    size_t rl = strlen(root);
    if (_strnicmp(win, root, (int)rl) == 0) {
        const char *rest = win + rl;              /* "\etc\units" ou "" */
        if (!*rest) { snprintf(buf, cap, "/"); }
        else        { snprintf(buf, cap, "%s", rest); }
        for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
    } else {
        snprintf(buf, cap, "%s", win);            /* fora da raiz: mostra Win32 */
        for (char *c = buf; *c; c++) if (*c == '\\') *c = '/';
    }
    return buf;
}

const char *npx_strerror(void)
{
    static char msg[256];
    DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM |
                             FORMAT_MESSAGE_IGNORE_INSERTS, NULL, g_err,
                             0, msg, sizeof msg - 1, NULL);
    if (!n) snprintf(msg, sizeof msg, "erro %lu", g_err);
    else { msg[n] = 0; for (char *c=msg; *c; c++) if (*c=='\r'||*c=='\n') { *c=0; break; } }
    return msg;
}
