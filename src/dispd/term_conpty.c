/*
 * term_conpty.c — backend de terminal via ConPTY (caminho primario).
 *
 * Compilado a 0x0A00 (headers do ConPTY) mas resolve as 3 funcoes via
 * GetProcAddress no kernel32 — assim, se estiverem ausentes (WinPE fino), o
 * start() falha limpo e o term.c cai no scrape, sem quebrar o carregamento.
 *
 * Referencias reais: PuTTY windows/conpty.c (init_conpty_api / wiring / teardown),
 * microsoft/terminal EchoCon.cpp (STARTUPINFOEX + attribute list).
 */
#include "term.h"

/* HPCON e PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE vem dos headers (0x0A00). */
typedef HRESULT (WINAPI *t_CreatePseudoConsole)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef HRESULT (WINAPI *t_ResizePseudoConsole)(HPCON, COORD);
typedef void    (WINAPI *t_ClosePseudoConsole)(HPCON);

static t_CreatePseudoConsole p_Create;
static t_ResizePseudoConsole p_Resize;
static t_ClosePseudoConsole  p_Close;

static int init_conpty_api(void)
{
    static int tried, ok;
    if (!tried) {
        tried = 1;
        HMODULE k = GetModuleHandleA("kernel32.dll");
        if (k) {
            p_Create = (t_CreatePseudoConsole)(void *)GetProcAddress(k, "CreatePseudoConsole");
            p_Resize = (t_ResizePseudoConsole)(void *)GetProcAddress(k, "ResizePseudoConsole");
            p_Close  = (t_ClosePseudoConsole)(void *)GetProcAddress(k, "ClosePseudoConsole");
        }
        ok = (p_Create && p_Resize && p_Close);
    }
    return ok;
}

typedef struct {
    HPCON  hpc;
    HANDLE in_w;    /* host escreve teclas aqui   */
    HANDLE out_r;   /* host le o stream VT aqui    */
    HANDLE hproc;
    HANDLE reader;
    volatile LONG stop;
    CRITICAL_SECTION in_lock;   /* serializa escritas no in_w: o main manda teclas
                                 * e a leitora manda respostas a queries (DSR/DA);
                                 * escritas concorrentes num pipe byte intercalam */
    int    in_lock_init;
} ConPty;

static DWORD WINAPI reader_main(LPVOID arg)
{
    Terminal *t = (Terminal *)arg;
    ConPty *c = (ConPty *)t->impl;
    char buf[4096];
    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(c->out_r, buf, sizeof buf, &n, NULL);
        if (!ok || n == 0)
            break;                 /* EOF: filho saiu / pipe fechado */
        vt_feed(t, buf, (int)n);
    }
    InterlockedExchange(&t->alive, 0);   /* #86 */
    return 0;
}

static int setup_startupinfo(STARTUPINFOEXA *si, HPCON hpc)
{
    ZeroMemory(si, sizeof *si);
    si->StartupInfo.cb = sizeof *si;

    SIZE_T sz = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &sz);
    si->lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)malloc(sz);
    if (!si->lpAttributeList)
        return -1;
    if (!InitializeProcThreadAttributeList(si->lpAttributeList, 1, 0, &sz)) {
        free(si->lpAttributeList);
        si->lpAttributeList = NULL;
        return -1;
    }
    if (!UpdateProcThreadAttribute(si->lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   hpc, sizeof(HPCON), NULL, NULL)) {
        DeleteProcThreadAttributeList(si->lpAttributeList);
        free(si->lpAttributeList);
        si->lpAttributeList = NULL;
        return -1;
    }
    return 0;
}

static int conpty_start(Terminal *t, const char *cmdline, int cols, int rows)
{
    if (!init_conpty_api())
        return -1;

    ConPty *c = (ConPty *)calloc(1, sizeof *c);
    if (!c)
        return -1;
    t->impl = c;
    t->backend_is_pty = 1;   /* aceita respostas a DSR/DA (write-back destrava ash) */
    InitializeCriticalSection(&c->in_lock);
    c->in_lock_init = 1;

    HANDLE in_r = INVALID_HANDLE_VALUE, out_w = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&in_r, &c->in_w, NULL, 0) ||
        !CreatePipe(&c->out_r, &out_w, NULL, 0)) {
        goto fail;
    }

    COORD size;
    size.X = (SHORT)(cols > 0 ? cols : 80);
    size.Y = (SHORT)(rows > 0 ? rows : 24);
    if (FAILED(p_Create(size, in_r, out_w, 0, &c->hpc)))
        goto fail;

    /* entregou os lados PTY -> host solta as pontas PTY */
    CloseHandle(in_r);  in_r = INVALID_HANDLE_VALUE;
    CloseHandle(out_w); out_w = INVALID_HANDLE_VALUE;

    STARTUPINFOEXA si;
    if (setup_startupinfo(&si, c->hpc) != 0)
        goto fail;

    char cmd[1024];
    strncpy(cmd, cmdline ? cmdline : "cmd.exe", sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;

    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                             EXTENDED_STARTUPINFO_PRESENT, NULL, ntu_root(),
                             &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    free(si.lpAttributeList);
    if (!ok)
        goto fail;

    CloseHandle(pi.hThread);
    c->hproc = pi.hProcess;
    t->pid = pi.dwProcessId;

    /* liga o libvterm ANTES da leitora, senao os primeiros bytes se perdem */
    if (vt_use_libvterm(t) != 0) {
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
    c->reader = CreateThread(NULL, 0, reader_main, t, 0, NULL);
    if (!c->reader) {              /* sem leitora o terminal fica morto */
        TerminateProcess(pi.hProcess, 1);
        goto fail;
    }
    return 0;

fail:
    if (in_r != INVALID_HANDLE_VALUE)  CloseHandle(in_r);
    if (out_w != INVALID_HANDLE_VALUE) CloseHandle(out_w);
    if (c->hpc)   p_Close(c->hpc);
    if (c->in_w)  CloseHandle(c->in_w);
    if (c->out_r) CloseHandle(c->out_r);
    if (c->hproc) CloseHandle(c->hproc);
    if (c->in_lock_init) DeleteCriticalSection(&c->in_lock);
    free(c);
    t->impl = NULL;
    return -1;
}

static void conpty_input(Terminal *t, const char *bytes, int n)
{
    ConPty *c = (ConPty *)t->impl;
    if (!c || !c->in_w)
        return;
    DWORD w;
    EnterCriticalSection(&c->in_lock);   /* main (teclas) x leitora (respostas) */
    WriteFile(c->in_w, bytes, (DWORD)n, &w, NULL);
    LeaveCriticalSection(&c->in_lock);
}

static void conpty_resize(Terminal *t, int cols, int rows)
{
    ConPty *c = (ConPty *)t->impl;
    if (!c || !p_Resize)
        return;
    COORD size;
    size.X = (SHORT)(cols > 0 ? cols : 1);
    size.Y = (SHORT)(rows > 0 ? rows : 1);
    HRESULT hr = p_Resize(c->hpc, size);   /* reader continua vivo -> sem deadlock */
    if (FAILED(hr))                        /* #90 */
        dispd_log("conpty: ResizePseudoConsole falhou (0x%lx)", (unsigned long)hr);
}

static void conpty_close(Terminal *t)
{
    ConPty *c = (ConPty *)t->impl;
    if (!c)
        return;
    /* ClosePseudoConsole primeiro (pode gerar output final), depois drena
     * (reader chega no EOF e sai), depois fecha os pipes. */
    if (c->hpc)
        p_Close(c->hpc);
    /* fecha a ponta de leitura ANTES de esperar: desbloqueia a thread leitora
     * na hora (ReadFile -> EOF). */
    if (c->out_r) { CloseHandle(c->out_r); c->out_r = NULL; }
    if (c->reader) {
        /* join GARANTIDO antes do free — sem isso o reader ainda usaria o
         * Terminal ja liberado (#87). Se travar (ex.: escrevendo resposta a
         * uma query no in_w cheio), destrava matando o filho e fechando in_w. */
        if (WaitForSingleObject(c->reader, 2000) != WAIT_OBJECT_0) {
            if (c->hproc) TerminateProcess(c->hproc, 0);
            if (c->in_w) { CloseHandle(c->in_w); c->in_w = NULL; }
            WaitForSingleObject(c->reader, INFINITE);
        }
        CloseHandle(c->reader);
    }
    if (c->in_w)  CloseHandle(c->in_w);
    if (c->hproc) {
        TerminateProcess(c->hproc, 0);   /* #93: garante o fim do filho */
        CloseHandle(c->hproc);
    }
    if (c->in_lock_init) DeleteCriticalSection(&c->in_lock);   /* apos o join */
    free(c);
    t->impl = NULL;
}

TerminalBackend term_conpty_backend = {
    "conpty", conpty_start, conpty_input, conpty_resize, conpty_close
};
