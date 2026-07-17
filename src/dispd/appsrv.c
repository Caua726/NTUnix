/*
 * appsrv.c — fronteira apps <-> dispd (M4).
 *
 * Apps GUI sao processos separados. Cada um conecta em NTU_PIPE_DISPD_APP,
 * pede uma superficie WxH; o dispd cria um section object nomeado e uma janela
 * WK_APP cujo DIB fica SOBRE essa section (mesma memoria — zero-copy, analogo
 * NT do wl_shm). O app abre a mesma section, desenha BGRA e manda APP-COMMIT;
 * o dispd marca a janela suja e recompoe. O ntwm tila a janela do app junto
 * com os terminais (recebe WINDOW-CREATED como qualquer outra).
 *
 * Servidor multi-cliente: uma thread de accept cria uma instancia de pipe por
 * conexao (PIPE_UNLIMITED_INSTANCES) e uma thread worker por app. Toda mutacao
 * de janela/GDI acontece no MAIN thread via fila (appsrv_drain).
 *
 * Protocolo (linhas ASCII, message-mode):
 *   app  -> dispd : APP-HELLO <w> <h> <titulo...>
 *   dispd-> app   : APP-SURFACE <nome-da-section> <w> <h>
 *   app  -> dispd : APP-COMMIT            (desenhou; recompoe)
 *   app  -> dispd : APP-CLOSE / desconecta
 */
#include "dispd.h"

typedef struct AppConn {
    volatile LONG wid;   /* preenchido pelo main no CREATE */
} AppConn;

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
static volatile LONG g_appctr;

static void aq_push(const AqItem *it)
{
    EnterCriticalSection(&g_aqlock);
    int nt = (g_aqt + 1) % AQCAP;
    if (nt != g_aqh) {
        g_aq[g_aqt] = *it;
        g_aqt = nt;
    }
    LeaveCriticalSection(&g_aqlock);
}

static int aq_pop(AqItem *out)
{
    int got = 0;
    EnterCriticalSection(&g_aqlock);
    if (g_aqh != g_aqt) {
        *out = g_aq[g_aqh];
        g_aqh = (g_aqh + 1) % AQCAP;
        got = 1;
    }
    LeaveCriticalSection(&g_aqlock);
    return got;
}

/* main thread: aplica os pedidos dos apps */
void appsrv_drain(void)
{
    AqItem it;
    while (aq_pop(&it)) {
        if (it.type == AQ_CREATE) {
            Window *w = win_create_shared(it.w, it.h, it.section);
            if (w) {
                strncpy(w->title, it.title, sizeof w->title - 1);
                w->title[sizeof w->title - 1] = 0;
                InterlockedExchange(&it.conn->wid, (LONG)w->id);
                win_focus(w);
                wmproto_ev_created(w);
                dispd_log("app conectado (id %u, %dx%d, %s)",
                          w->id, it.w, it.h, it.title);
            } else if (it.section) {
                CloseHandle(it.section);
            }
        } else if (it.type == AQ_COMMIT) {
            Window *w = win_find((unsigned)it.conn->wid);
            if (w) {
                w->dirty = 1;
                g_srv.dirty = 1;
            }
        } else { /* AQ_DESTROY */
            unsigned id = (unsigned)it.conn->wid;
            Window *w = win_find(id);
            if (w) {
                win_destroy(w);
                wmproto_ev_destroyed(id);
            }
            free(it.conn);
        }
    }
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    HANDLE pipe = (HANDLE)arg;
    AppConn *conn = (AppConn *)calloc(1, sizeof *conn);
    if (!conn) {
        CloseHandle(pipe);
        return 0;
    }
    int created = 0;
    char buf[512];

    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(pipe, buf, sizeof buf - 1, &n, NULL);
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
                                            PAGE_READWRITE, 0,
                                            (DWORD)(w * h * 4), name);
            if (!sec)
                break;

            char resp[128];
            int rn = snprintf(resp, sizeof resp, "APP-SURFACE %s %d %d", name, w, h);
            DWORD wr;
            WriteFile(pipe, resp, (DWORD)rn, &wr, NULL);

            AqItem it;
            ZeroMemory(&it, sizeof it);
            it.type = AQ_CREATE;
            it.section = sec;
            it.w = w;
            it.h = h;
            strncpy(it.title, title, sizeof it.title - 1);
            it.conn = conn;
            aq_push(&it);
            created = 1;
        } else if (!strncmp(buf, "APP-COMMIT", 10)) {
            AqItem it;
            ZeroMemory(&it, sizeof it);
            it.type = AQ_COMMIT;
            it.conn = conn;
            aq_push(&it);
        } else if (!strncmp(buf, "APP-CLOSE", 9)) {
            break;
        }
    }

    CloseHandle(pipe);
    /* pede destruicao; o main libera conn */
    AqItem it;
    ZeroMemory(&it, sizeof it);
    it.type = AQ_DESTROY;
    it.conn = conn;
    aq_push(&it);
    return 0;
}

static DWORD WINAPI accept_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(NTU_PIPE_DISPD_APP, PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                       PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
                                       65536, 65536, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }
        BOOL con = ConnectNamedPipe(pipe, NULL)
                       ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!con) {
            CloseHandle(pipe);
            continue;
        }
        HANDLE t = CreateThread(NULL, 0, worker_main, pipe, 0, NULL);
        if (t)
            CloseHandle(t);
        else
            CloseHandle(pipe);
    }
    return 0;
}

void appsrv_start(void)
{
    InitializeCriticalSection(&g_aqlock);
    HANDLE t = CreateThread(NULL, 0, accept_main, NULL, 0, NULL);
    if (t)
        CloseHandle(t);
    dispd_log("appsrv: ouvindo em %s", NTU_PIPE_DISPD_APP);
}
