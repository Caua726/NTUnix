/*
 * appsrv.c — fronteira apps <-> dispd (M4).
 *
 * Apps GUI sao processos separados que conectam em NTU_PIPE_DISPD_APP, pedem
 * uma superficie WxH; o dispd cria um section object nomeado e uma janela
 * WK_APP cujo DIB fica SOBRE a section (zero-copy, analogo NT do wl_shm). O app
 * desenha e manda APP-COMMIT; o dispd recompoe. O ntwm tila junto dos terminais.
 *
 * Servidor multi-cliente: thread de accept cria uma instancia de pipe por app
 * (PIPE_UNLIMITED_INSTANCES, OVERLAPPED) e uma thread worker por app. Toda
 * mutacao de janela/GDI ocorre no MAIN thread via fila (appsrv_drain). Um
 * registro de conexoes permite o main enviar entrada (APP-KEY/APP-MOUSE) e
 * fechar apps (appsrv_close_wid).
 *
 * Protocolo (linhas ASCII, message-mode):
 *   app  -> dispd : APP-HELLO <w> <h> <titulo...>
 *   dispd-> app   : APP-SURFACE <nome-section> <w> <h>   (so apos a janela existir, #46)
 *   dispd-> app   : APP-ERR <motivo>                     (falha)
 *   app  -> dispd : APP-COMMIT
 *   dispd-> app   : APP-KEY <mods> <vk> <ch>   /  APP-MOUSE <x> <y> <botoes>
 *   app  -> dispd : APP-CLOSE / desconecta
 */
#include "dispd.h"

typedef struct AppConn {
    volatile LONG wid;
    HANDLE pipe;
    HANDLE rev, wev, ready;   /* eventos: read, write, ready(main confirmou) */
    CRITICAL_SECTION wlock;   /* serializa escritas do main/worker */
    volatile LONG dead;
    volatile LONG created;    /* 0=pendente, 1=ok, -1=falhou */
    struct AppConn *next;
} AppConn;

static AppConn *g_conns;
static CRITICAL_SECTION g_conns_lock;
static volatile LONG g_appctr;

typedef enum { AQ_CREATE, AQ_COMMIT, AQ_DESTROY } AqType;
typedef struct {
    AqType   type;
    HANDLE   section;
    int      w, h;
    char     title[128];
    AppConn *conn;
} AqItem;

#define AQCAP 256
static AqItem g_aq[AQCAP];
static int g_aqh, g_aqt;
static CRITICAL_SECTION g_aqlock;

static void aq_push(const AqItem *it)
{
    EnterCriticalSection(&g_aqlock);
    int nt = (g_aqt + 1) % AQCAP;
    if (nt != g_aqh) { g_aq[g_aqt] = *it; g_aqt = nt; }
    LeaveCriticalSection(&g_aqlock);
}

static int aq_pop(AqItem *out)
{
    int got = 0;
    EnterCriticalSection(&g_aqlock);
    if (g_aqh != g_aqt) { *out = g_aq[g_aqh]; g_aqh = (g_aqh + 1) % AQCAP; got = 1; }
    LeaveCriticalSection(&g_aqlock);
    return got;
}

/* ---- registro de conexoes ---- */

static void conns_add(AppConn *c)
{
    EnterCriticalSection(&g_conns_lock);
    c->next = g_conns; g_conns = c;
    LeaveCriticalSection(&g_conns_lock);
}

static void conns_remove(AppConn *c)
{
    EnterCriticalSection(&g_conns_lock);
    AppConn **pp = &g_conns;
    while (*pp && *pp != c) pp = &(*pp)->next;
    if (*pp) *pp = c->next;
    LeaveCriticalSection(&g_conns_lock);
}

static AppConn *conns_find(unsigned wid)
{
    AppConn *r = NULL;
    EnterCriticalSection(&g_conns_lock);
    for (AppConn *c = g_conns; c; c = c->next)
        if ((unsigned)c->wid == wid) { r = c; break; }
    LeaveCriticalSection(&g_conns_lock);
    return r;
}

/* escrita ao app (overlapped, com timeout) — worker (setup) e main (input) */
static void app_send(AppConn *c, const char *line)
{
    if (!c || InterlockedCompareExchange(&c->dead, 0, 0))
        return;
    EnterCriticalSection(&c->wlock);
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof ov);
    ov.hEvent = c->wev;
    ResetEvent(c->wev);
    DWORD w = 0;
    BOOL ok = WriteFile(c->pipe, line, (DWORD)strlen(line), &w, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(c->wev, 1000) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(c->pipe, &ov, &w, FALSE);
        else {
            /* espera o cancel terminar: 'ov' e 'line' sao locais e o kernel
             * ainda pode escrever neles ate a I/O cancelada completar (#60). */
            CancelIoEx(c->pipe, &ov);
            GetOverlappedResult(c->pipe, &ov, &w, TRUE);
            ok = FALSE;
        }
    }
    LeaveCriticalSection(&c->wlock);
    if (!ok)
        InterlockedExchange(&c->dead, 1);
}

void appsrv_input_key(unsigned id, unsigned mods, unsigned vk, unsigned ch, int down)
{
    AppConn *c = conns_find(id);
    if (c) { char b[64]; snprintf(b, sizeof b, "APP-KEY %x %x %x %d", mods, vk, ch, down ? 1 : 0);
             app_send(c, b); }   /* #10: acao down/up */
}

void appsrv_input_mouse(unsigned id, int x, int y, int buttons)
{
    AppConn *c = conns_find(id);
    if (c) { char b[64]; snprintf(b, sizeof b, "APP-MOUSE %d %d %d", x, y, buttons); app_send(c, b); }
}

void appsrv_close_wid(unsigned id)
{
    AppConn *c = conns_find(id);
    if (c) CancelIoEx(c->pipe, NULL);   /* desbloqueia a leitora -> worker sai */
}

/* ---- main thread: aplica os pedidos ---- */

void appsrv_drain(void)
{
    AqItem it;
    while (aq_pop(&it)) {
        if (it.type == AQ_CREATE) {
            Window *w = win_create_shared(it.w, it.h, it.section);
            if (w) {
                strncpy(w->title, it.title, sizeof w->title - 1);
                w->title[sizeof w->title - 1] = 0;
                for (char *p = w->title; *p; p++)          /* #17 sem injecao */
                    if ((unsigned char)*p < 0x20) *p = ' ';
                InterlockedExchange(&it.conn->wid, (LONG)w->id);
                InterlockedExchange(&it.conn->created, 1);
                win_focus(w);
                wmproto_ev_created(w);
                dispd_log("app conectado (id %u, %dx%d, %s)", w->id, it.w, it.h, it.title);
            } else {
                if (it.section) CloseHandle(it.section);
                InterlockedExchange(&it.conn->created, -1);
            }
            SetEvent(it.conn->ready);   /* libera o worker p/ APP-SURFACE/ERR (#46) */
        } else if (it.type == AQ_COMMIT) {
            Window *w = win_find((unsigned)it.conn->wid);
            if (w) { w->dirty = 1; g_srv.dirty = 1; }
        } else { /* AQ_DESTROY: worker ja saiu e removeu do registro */
            unsigned id = (unsigned)it.conn->wid;
            Window *w = win_find(id);
            if (w) { win_destroy(w); wmproto_ev_destroyed(id); }
            CloseHandle(it.conn->rev);
            CloseHandle(it.conn->wev);
            CloseHandle(it.conn->ready);
            DeleteCriticalSection(&it.conn->wlock);
            free(it.conn);
        }
    }
}

/* ---- worker por app (overlapped reads) ---- */

static DWORD WINAPI worker_main(LPVOID arg)
{
    HANDLE pipe = (HANDLE)arg;
    AppConn *c = (AppConn *)calloc(1, sizeof *c);
    if (!c) { CloseHandle(pipe); return 0; }
    c->pipe = pipe;
    c->rev = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->wev = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    InitializeCriticalSection(&c->wlock);
    conns_add(c);

    int created = 0;
    char buf[512];
    for (;;) {
        DWORD n = 0;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = c->rev;
        ResetEvent(c->rev);
        BOOL ok = ReadFile(pipe, buf, sizeof buf - 1, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING)
            ok = GetOverlappedResult(pipe, &ov, &n, TRUE);
        if ((!ok && GetLastError() != ERROR_MORE_DATA) || n == 0)
            break;
        buf[n] = 0;

        if (!strncmp(buf, "APP-HELLO", 9) && !created) {
            char *p = buf + 9;
            int w = (int)strtol(p, &p, 10);
            int h = (int)strtol(p, &p, 10);
            while (*p == ' ' || *p == '\t') p++;
            char title[128] = "app";
            if (*p) { strncpy(title, p, sizeof title - 1); title[sizeof title - 1] = 0; }
            ntu_trim(title);
            if (w < 1) w = 200;
            if (h < 1) h = 100;
            if (w > 4096) w = 4096;
            if (h > 4096) h = 4096;

            char name[64];
            LONG id = InterlockedIncrement(&g_appctr);
            snprintf(name, sizeof name, "ntunix-appsurf-%ld", (long)id);
            HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                            PAGE_READWRITE, 0, (DWORD)(w * h * 4), name);
            if (!sec) { app_send(c, "APP-ERR no-surface"); break; }

            /* enfileira CREATE e ESPERA o main confirmar antes de dar a surface (#46) */
            AqItem it;
            ZeroMemory(&it, sizeof it);
            it.type = AQ_CREATE; it.section = sec; it.w = w; it.h = h;
            strncpy(it.title, title, sizeof it.title - 1);
            it.conn = c;
            ResetEvent(c->ready);
            aq_push(&it);
            WaitForSingleObject(c->ready, 5000);

            if (InterlockedCompareExchange(&c->created, 0, 0) == 1) {
                char resp[128];
                snprintf(resp, sizeof resp, "APP-SURFACE %s %d %d", name, w, h);
                app_send(c, resp);
                created = 1;
            } else {
                app_send(c, "APP-ERR create-failed");
                break;
            }
        } else if (!strncmp(buf, "APP-COMMIT", 10)) {
            AqItem it;
            ZeroMemory(&it, sizeof it);
            it.type = AQ_COMMIT; it.conn = c;
            aq_push(&it);
        } else if (!strncmp(buf, "APP-CLOSE", 9)) {
            break;
        }
    }

    InterlockedExchange(&c->dead, 1);
    conns_remove(c);
    CloseHandle(pipe);
    c->pipe = NULL;

    AqItem it;
    ZeroMemory(&it, sizeof it);
    it.type = AQ_DESTROY; it.conn = c;
    aq_push(&it);    /* main libera os recursos de c */
    return 0;
}

static DWORD WINAPI accept_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(NTU_PIPE_DISPD_APP,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                       PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       65536, 65536, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        HANDLE cev = CreateEventA(NULL, TRUE, FALSE, NULL);
        OVERLAPPED cov;
        ZeroMemory(&cov, sizeof cov);
        cov.hEvent = cev;
        BOOL con = ConnectNamedPipe(pipe, &cov)
                       ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!con && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(cev, INFINITE);
            con = TRUE;
        }
        CloseHandle(cev);
        if (!con) { CloseHandle(pipe); continue; }

        HANDLE t = CreateThread(NULL, 0, worker_main, pipe, 0, NULL);
        if (t) CloseHandle(t);
        else CloseHandle(pipe);
    }
    return 0;
}

void appsrv_start(void)
{
    InitializeCriticalSection(&g_aqlock);
    InitializeCriticalSection(&g_conns_lock);
    HANDLE t = CreateThread(NULL, 0, accept_main, NULL, 0, NULL);
    if (t) CloseHandle(t);
    dispd_log("appsrv: ouvindo em %s", NTU_PIPE_DISPD_APP);
}
