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
    HANDLE hjob;     /* audit #64: Job Object -> mata a arvore do shell no close */
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

/* "X:\Windows\System32" -> "/mnt/x/Windows/System32" (drive estilo WSL) */
static void win2unix(const char *w, char *out, size_t cap)
{
    if (!w[0] || w[1] != ':') { out[0] = 0; return; }
    char drive = (w[0] >= 'A' && w[0] <= 'Z') ? (char)(w[0] + 32) : w[0];
    int n = snprintf(out, cap, "/mnt/%c", drive);
    for (const char *q = w + 2; *q && n < (int)cap - 1; q++)
        out[n++] = (*q == '\\') ? '/' : *q;
    out[n] = 0;
}

/* bloco de env pro filho = env do dispd (menos PATH) + NTU_PTY/NTU_PTY_WS +
 * PATH unix-form (o ash divide em ':' e usa caminhos unix; a PATH do Windows
 * — ';' e 'X:\' — e' inutilizavel por ele). Inclui o System32 via /mnt pra
 * achar cmd/powershell/taskmgr direto do shell. */
static char *build_env(const char *ws_name)
{
    char *base = GetEnvironmentStringsA();
    if (!base)
        return NULL;

    char sysdir[MAX_PATH] = "", windir[MAX_PATH] = "", usys[MAX_PATH], uwin[MAX_PATH];
    GetSystemDirectoryA(sysdir, sizeof sysdir);
    GetWindowsDirectoryA(windir, sizeof windir);
    win2unix(sysdir, usys, sizeof usys);
    win2unix(windir, uwin, sizeof uwin);
    char upath[MAX_PATH * 3];
    snprintf(upath, sizeof upath, "PATH=/system/bin:%s:%s",
             usys[0] ? usys : "/mnt/x/windows/system32",
             uwin[0] ? uwin : "/mnt/x/windows");

    char extra2[96];
    int e2 = snprintf(extra2, sizeof extra2, "NTU_PTY_WS=%s", ws_name);
    if (e2 < 0) { FreeEnvironmentStringsA(base); return NULL; }

    size_t kept = 0;   /* soma das entradas mantidas (pula PATH=) */
    for (char *e = base; *e; e += strlen(e) + 1)
        if (_strnicmp(e, "PATH=", 5) != 0)
            kept += strlen(e) + 1;

    size_t total = kept + sizeof("NTU_PTY=1") + (size_t)e2 + 1 +
                   strlen(upath) + 1 + 1;
    char *out = (char *)malloc(total);
    if (out) {
        size_t p = 0;
        for (char *e = base; *e; e += strlen(e) + 1) {
            if (_strnicmp(e, "PATH=", 5) == 0) continue;
            size_t l = strlen(e) + 1;
            memcpy(out + p, e, l); p += l;
        }
        memcpy(out + p, "NTU_PTY=1", sizeof("NTU_PTY=1")); p += sizeof("NTU_PTY=1");
        memcpy(out + p, extra2, (size_t)e2 + 1);           p += (size_t)e2 + 1;
        memcpy(out + p, upath, strlen(upath) + 1);         p += strlen(upath) + 1;
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

    /* winsize compartilhada com o slave (a musl-nt le no TIOCGWINSZ).
     * audit #110: nome com sal (menos previsivel) + checa colisao + aborta se a
     * view falhar (antes seguia com c->ws NULL e o slave recebia lixo/default). */
    char wsname[80];
    unsigned salt = (unsigned)GetTickCount() ^ (GetCurrentProcessId() << 8) ^
                    ((unsigned)InterlockedIncrement(&g_pty_ctr) * 2654435761u);
    snprintf(wsname, sizeof wsname, "ntunix-pty-ws-%lu-%08x",
             GetCurrentProcessId(), salt);
    c->ws_map = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, 4, wsname);
    if (!c->ws_map)
        goto fail;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {   /* nome ja existia -> colisao */
        CloseHandle(c->ws_map); c->ws_map = NULL;
        goto fail;
    }
    c->ws = (volatile unsigned short *)MapViewOfFile(c->ws_map, FILE_MAP_WRITE, 0, 0, 4);
    if (!c->ws)
        goto fail;
    c->ws[0] = (unsigned short)(cols > 0 ? cols : 80);
    c->ws[1] = (unsigned short)(rows > 0 ? rows : 24);

    /* pipes: pontas do filho herdaveis; pontas do master nao */
    SECURITY_ATTRIBUTES sa;
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = NULL;
    sa.bInheritHandle = TRUE;
    /* audit #65: buffer grande no pipe de STDIN (master->filho) para o WriteFile
     * sincrono de pty_input (rodado no MAIN thread ao teclar) so bloquear se o
     * filho parar de ler o stdin E chegarem >64K de input — na pratica nunca no
     * uso interativo. Fix completo (writer thread/queue) fica como follow-up. */
    if (!CreatePipe(&in_r, &c->in_w, &sa, 65536))   /* in_r=filho(stdin) in_w=master */
        goto fail;
    if (!CreatePipe(&c->out_r, &out_w, &sa, 0))     /* out_r=master out_w=filho(stdout) */
        goto fail;
    SetHandleInformation(c->in_w, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(c->out_r, HANDLE_FLAG_INHERIT, 0);

    env = build_env(wsname);
    if (!env)
        goto fail;

    char cmd[1024];
    strncpy(cmd, cmdline ? cmdline : "cmd.exe", sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;

    /* audit #111: bInheritHandles=TRUE sozinho herda TODOS os handles herdaveis
     * -> um novo terminal herdaria as pontas de pipe de OUTROS terminais e o EOF
     * nunca chegaria neles. Passa uma lista EXPLICITA: so as 2 pontas do filho. */
    STARTUPINFOEXA six;
    ZeroMemory(&six, sizeof six);
    six.StartupInfo.cb = sizeof six;
    six.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    six.StartupInfo.hStdInput = in_r;
    six.StartupInfo.hStdOutput = out_w;
    six.StartupInfo.hStdError = out_w;

    HANDLE inherit_list[2] = { in_r, out_w };
    SIZE_T attr_sz = 0;
    LPPROC_THREAD_ATTRIBUTE_LIST attrs = NULL;
    DWORD cflags = CREATE_NO_WINDOW | CREATE_SUSPENDED;   /* #64: atribui ao job antes de rodar */
    InitializeProcThreadAttributeList(NULL, 1, 0, &attr_sz);
    if (attr_sz && (attrs = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attr_sz)) &&
        InitializeProcThreadAttributeList(attrs, 1, 0, &attr_sz) &&
        UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                  inherit_list, sizeof inherit_list, NULL, NULL)) {
        six.lpAttributeList = attrs;
        cflags |= EXTENDED_STARTUPINFO_PRESENT;
    } else if (attrs) {
        free(attrs);
        attrs = NULL;   /* fallback: sem lista (comportamento antigo) */
    }

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, cflags,
                             env, ntu_root(), &six.StartupInfo, &pi);
    if (attrs) { DeleteProcThreadAttributeList(attrs); free(attrs); }
    free(env);
    env = NULL;
    /* o master nao precisa das pontas do filho */
    CloseHandle(in_r);  in_r = NULL;
    CloseHandle(out_w); out_w = NULL;
    if (!ok)
        goto fail;

    /* audit #64: cria o Job, atribui o filho e SO ENTAO resume -> o ash e seus
     * descendentes ficam no job e morrem juntos ao fechar (KILL_ON_JOB_CLOSE). */
    c->hjob = CreateJobObjectA(NULL, NULL);
    if (c->hjob) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli;
        ZeroMemory(&jeli, sizeof jeli);
        jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(c->hjob, JobObjectExtendedLimitInformation, &jeli, sizeof jeli);
        AssignProcessToJobObject(c->hjob, pi.hProcess);
    }
    if (ResumeThread(pi.hThread) == (DWORD)-1) {   /* CREATE_SUSPENDED -> resume */
        CloseHandle(pi.hThread);
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
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
    if (c->hjob)   { TerminateJobObject(c->hjob, 1); CloseHandle(c->hjob); }
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
    if (c->hjob)                          /* audit #64: mata a ARVORE (ash + descendentes) */
        TerminateJobObject(c->hjob, 0);
    else if (c->hproc)
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
    if (c->hjob)   CloseHandle(c->hjob);   /* audit #64 */
    if (c->ws)     UnmapViewOfFile((void *)c->ws);
    if (c->ws_map) CloseHandle(c->ws_map);
    if (c->in_lock_init) DeleteCriticalSection(&c->in_lock);
    free(c);
    t->impl = NULL;
}

TerminalBackend term_pty_backend = {
    "pty", pty_start, pty_input, pty_resize, pty_close
};
