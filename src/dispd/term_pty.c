/*
 * term_pty.c — backend de terminal: PTY nativo do NTUnix (sem ConPTY).
 *
 * O Windows nao tem pty; a gente sintetiza a abstracao (modelo Cygwin). O dispd
 * e' o MASTER: cria dois pipes anonimos (stdin/stdout do filho), um file-mapping
 * de winsize e um bloco de env marcando NTU_PTY. Spawna o busybox com esses
 * pipes como std handles. A musl-nt (a libc do filho), vendo NTU_PTY, trata os
 * fds 0/1/2 como um tty SLAVE: isatty()=true, termios em memoria, winsize do
 * mapping, ONLCR na saida. Assim o ash entra em modo interativo e cospe um
 * stream VT limpo direto pro nosso pipe -> libvterm. Sem ConPTY, sem scraping,
 * multi-terminal nativo (cada terminal = um par de pipes).
 */
#include "term.h"

typedef struct {
    HANDLE in_w;     /* master escreve teclas aqui (stdin do filho)  */
    HANDLE out_r;    /* master le o stream VT aqui (stdout do filho) */
    HANDLE hproc;
    HANDLE reader;
    HANDLE ws_map;                 /* file-mapping da winsize (compart. c/ o slave) */
    volatile unsigned short *ws;   /* view: [0]=cols [1]=rows */
    CRITICAL_SECTION in_lock;      /* serializa escritas no in_w (main x leitora) */
    int    in_lock_init;
} Pty;

static volatile LONG g_pty_ctr;

static DWORD WINAPI reader_main(LPVOID arg)
{
    Terminal *t = (Terminal *)arg;
    Pty *c = (Pty *)t->impl;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        if (!ReadFile(c->out_r, buf, sizeof buf, &n, NULL) || n == 0)
            break;                 /* EOF: filho saiu */
        vt_feed(t, buf, (int)n);
    }
    InterlockedExchange(&t->alive, 0);
    return 0;
}

/* bloco de env = env do dispd + NTU_PTY=1 + NTU_PTY_WS=<nome do mapping> */
static char *build_env(const char *ws_name)
{
    char *base = GetEnvironmentStringsA();
    if (!base)
        return NULL;
    size_t k = 0;
    while (!(base[k] == 0 && base[k + 1] == 0)) k++;
    size_t keep = (base[0] == 0) ? 0 : k + 1;   /* "..varN\0" (sem o \0 final) */

    char extra2[96];
    int e2 = snprintf(extra2, sizeof extra2, "NTU_PTY_WS=%s", ws_name);
    if (e2 < 0) { FreeEnvironmentStringsA(base); return NULL; }

    size_t total = keep + sizeof("NTU_PTY=1") + (size_t)e2 + 1 + 1;
    char *out = (char *)malloc(total);
    if (out) {
        size_t p = 0;
        memcpy(out, base, keep);              p += keep;
        memcpy(out + p, "NTU_PTY=1", sizeof("NTU_PTY=1")); p += sizeof("NTU_PTY=1");
        memcpy(out + p, extra2, (size_t)e2 + 1);           p += (size_t)e2 + 1;
        out[p] = 0;                            /* terminador duplo do bloco */
    }
    FreeEnvironmentStringsA(base);
    return out;
}

static int pty_start(Terminal *t, const char *cmdline, int cols, int rows)
{
    Pty *c = (Pty *)calloc(1, sizeof *c);
    if (!c)
        return -1;
    t->impl = c;
    t->backend_is_pty = 1;

    HANDLE in_r = NULL, out_w = NULL;
    char *env = NULL;

    /* winsize compartilhada com o slave (a musl-nt le no TIOCGWINSZ) */
    char wsname[64];
    snprintf(wsname, sizeof wsname, "ntunix-pty-ws-%lu-%ld",
             GetCurrentProcessId(), (long)InterlockedIncrement(&g_pty_ctr));
    c->ws_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4, wsname);
    if (!c->ws_map)
        goto fail;
    c->ws = (volatile unsigned short *)MapViewOfFile(c->ws_map, FILE_MAP_WRITE, 0, 0, 4);
    if (c->ws) {
        c->ws[0] = (unsigned short)(cols > 0 ? cols : 80);
        c->ws[1] = (unsigned short)(rows > 0 ? rows : 24);
    }

    /* pipes: pontas do filho herdaveis; pontas do master nao */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&in_r, &c->in_w, &sa, 0))       /* in_r=filho(stdin) in_w=master */
        goto fail;
    if (!CreatePipe(&c->out_r, &out_w, &sa, 0))     /* out_r=master out_w=filho(stdout) */
        goto fail;
    SetHandleInformation(c->in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c->out_r, HANDLE_FLAG_INHERIT, 0);

    env = build_env(wsname);
    if (!env)
        goto fail;

    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = in_r;
    si.hStdOutput = out_w;
    si.hStdError = out_w;

    char cmd[1024];
    strncpy(cmd, cmdline ? cmdline : "cmd.exe", sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW,
                             env, ntu_root(), &si, &pi);
    free(env);
    env = NULL;
    /* o master nao precisa das pontas do filho */
    CloseHandle(in_r);  in_r = NULL;
    CloseHandle(out_w); out_w = NULL;
    if (!ok)
        goto fail;

    CloseHandle(pi.hThread);
    c->hproc = pi.hProcess;
    t->pid = pi.dwProcessId;
    InitializeCriticalSection(&c->in_lock);
    c->in_lock_init = 1;

    if (vt_use_libvterm(t) != 0) {   /* liga o libvterm antes da leitora */
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
    c->reader = CreateThread(NULL, 0, reader_main, t, 0, NULL);
    if (!c->reader) {
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
    return 0;

fail:
    if (in_r)  CloseHandle(in_r);
    if (out_w) CloseHandle(out_w);
    if (env)   free(env);
    if (c->in_w)   CloseHandle(c->in_w);
    if (c->out_r)  CloseHandle(c->out_r);
    if (c->hproc)  { TerminateProcess(c->hproc, 1); CloseHandle(c->hproc); }
    if (c->ws)     UnmapViewOfFile((void *)c->ws);
    if (c->ws_map) CloseHandle(c->ws_map);
    if (c->in_lock_init) DeleteCriticalSection(&c->in_lock);
    free(c);
    t->impl = NULL;
    return -1;
}

static void pty_input(Terminal *t, const char *bytes, int n)
{
    Pty *c = (Pty *)t->impl;
    if (!c || !c->in_w)
        return;
    DWORD w;
    EnterCriticalSection(&c->in_lock);   /* main (teclas) x leitora (respostas) */
    WriteFile(c->in_w, bytes, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&c->in_lock);
}

static void pty_resize(Terminal *t, int cols, int rows)
{
    Pty *c = (Pty *)t->impl;
    if (!c || !c->ws)
        return;
    /* atualiza a winsize compartilhada; o ash pega no proximo TIOCGWINSZ (a cada
     * prompt). SIGWINCH dinamico fica p/ depois (evita mexer no sys_signal.c). */
    c->ws[0] = (unsigned short)(cols > 0 ? cols : 1);
    c->ws[1] = (unsigned short)(rows > 0 ? rows : 1);
}

static void pty_close(Terminal *t)
{
    Pty *c = (Pty *)t->impl;
    if (!c)
        return;
    if (c->hproc)
        TerminateProcess(c->hproc, 0);   /* filho morre -> out_w fecha -> reader EOF */
    if (c->out_r) { CloseHandle(c->out_r); c->out_r = NULL; }
    if (c->reader) {
        if (WaitForSingleObject(c->reader, 2000) != WAIT_OBJECT_0) {
            if (c->in_w) { CloseHandle(c->in_w); c->in_w = NULL; }  /* destrava reply */
            WaitForSingleObject(c->reader, INFINITE);               /* join garantido */
        }
        CloseHandle(c->reader);
    }
    if (c->in_w)   CloseHandle(c->in_w);
    if (c->hproc)  CloseHandle(c->hproc);
    if (c->ws)     UnmapViewOfFile((void *)c->ws);
    if (c->ws_map) CloseHandle(c->ws_map);
    if (c->in_lock_init) DeleteCriticalSection(&c->in_lock);
    free(c);
    t->impl = NULL;
}

TerminalBackend term_pty_backend = {
    "pty", pty_start, pty_input, pty_resize, pty_close
};
