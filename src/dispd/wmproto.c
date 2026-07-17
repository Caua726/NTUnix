/*
 * wmproto.c — servidor do protocolo dispd <-> ntwm (NTU_PIPE_DISPD).
 *
 * Canal duplex persistente, message-mode, OVERLAPPED (obrigatorio: sem ele um
 * ReadFile bloqueado serializa e mata as escritas de evento — ref. MS
 * "Named Pipe Server Using Overlapped I/O"). Uma thread leitora enfileira os
 * comandos do ntwm; o main thread do dispd drena a fila (aplica) e escreve os
 * eventos. Um unico escritor (main thread) => sem lock de escrita.
 *
 * Estado de janela e' todo do dispd; se o ntwm cai, o desktop continua e um
 * ntwm novo recebe um snapshot (WINDOW... + SYNC) e re-declara o layout.
 */
#include "dispd.h"
#include "../common/ntuwm.h"

#define QCAP 512
#define GRABCAP 64

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_reader;
static HANDLE g_rev, g_wev, g_cev;     /* eventos: read, write, connect */
static OVERLAPPED g_wov;               /* overlapped do escritor (main thread) */
static volatile LONG g_connected;

/* fila de comandos (produtor: reader thread; consumidor: main thread) */
static char *g_q[QCAP];
static int g_qh, g_qt;
static CRITICAL_SECTION g_qlock;

/* tabela de grabs (so tocada pelo main thread) */
static struct { unsigned mods, vk; } g_grabs[GRABCAP];
static int g_ngrabs;

/* ---- fila ---- */

static void q_push(const char *s)
{
    char *dup = _strdup(s);
    if (!dup)
        return;
    EnterCriticalSection(&g_qlock);
    int nt = (g_qt + 1) % QCAP;
    if (nt == g_qh) {           /* cheia: descarta o mais novo */
        free(dup);
    } else {
        g_q[g_qt] = dup;
        g_qt = nt;
    }
    LeaveCriticalSection(&g_qlock);
}

static char *q_pop(void)
{
    char *s = NULL;
    EnterCriticalSection(&g_qlock);
    if (g_qh != g_qt) {
        s = g_q[g_qh];
        g_qh = (g_qh + 1) % QCAP;
    }
    LeaveCriticalSection(&g_qlock);
    return s;
}

/* ---- escrita de eventos (main thread) ---- */

static void wm_send(const char *line)
{
    if (!InterlockedCompareExchange(&g_connected, 0, 0))
        return;
    ResetEvent(g_wev);
    ZeroMemory(&g_wov, sizeof g_wov);
    g_wov.hEvent = g_wev;
    DWORD w = 0;
    BOOL ok = WriteFile(g_pipe, line, (DWORD)strlen(line), &w, &g_wov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = GetOverlappedResult(g_pipe, &g_wov, &w, TRUE);
    if (!ok)
        InterlockedExchange(&g_connected, 0);
}

int wmproto_connected(void)
{
    return (int)InterlockedCompareExchange(&g_connected, 0, 0);
}

void wmproto_ev_created(Window *w)
{
    char b[512];
    snprintf(b, sizeof b, "%s %u %d %lu %s", EVT_CREATED, w->id,
             (int)w->kind, w->pid, w->title[0] ? w->title : "terminal");
    wm_send(b);
}

void wmproto_ev_destroyed(unsigned id)
{
    char b[64];
    snprintf(b, sizeof b, "%s %u", EVT_DESTROYED, id);
    wm_send(b);
}

void wmproto_ev_title(Window *w)
{
    char b[512];
    snprintf(b, sizeof b, "%s %u %s", EVT_TITLE, w->id, w->title);
    wm_send(b);
}

void wmproto_ev_key(unsigned mods, unsigned vk)
{
    char b[64];
    snprintf(b, sizeof b, "%s %x %x", EVT_KEY, mods, vk);
    wm_send(b);
}

int wmproto_grabbed(unsigned mods, unsigned vk)
{
    for (int i = 0; i < g_ngrabs; i++)
        if (g_grabs[i].mods == mods && g_grabs[i].vk == vk)
            return 1;
    return 0;
}

static void grab_add(unsigned mods, unsigned vk)
{
    if (wmproto_grabbed(mods, vk) || g_ngrabs >= GRABCAP)
        return;
    g_grabs[g_ngrabs].mods = mods;
    g_grabs[g_ngrabs].vk = vk;
    g_ngrabs++;
}

static void grab_del(unsigned mods, unsigned vk)
{
    for (int i = 0; i < g_ngrabs; i++)
        if (g_grabs[i].mods == mods && g_grabs[i].vk == vk) {
            g_grabs[i] = g_grabs[--g_ngrabs];
            return;
        }
}

/* ---- handshake / snapshot (main thread) ---- */

static void send_snapshot(void)
{
    char b[512];
    snprintf(b, sizeof b, "%s dispd %d %s", EVT_WELCOME, NTUWM_PROTO_VER, ntu_root());
    wm_send(b);
    /* area util = tela menos a barra de status no topo */
    snprintf(b, sizeof b, "%s 0 0 %d %d %d", EVT_OUTPUT, g_srv.bar_h,
             g_srv.scr_w, g_srv.scr_h - g_srv.bar_h);
    wm_send(b);
    for (Window *w = g_srv.windows; w; w = w->next) {
        snprintf(b, sizeof b, "%s %u %d %lu %s", EVT_WINDOW, w->id,
                 (int)w->kind, w->pid, w->title[0] ? w->title : "terminal");
        wm_send(b);
    }
    wm_send(EVT_SYNC);
}

/* ---- aplicacao de um comando (main thread) ---- */

static void apply(char *line)
{
    char *av[10];
    int n = ntuwm_split(line, av, 10, -1);
    if (n < 1)
        return;
    const char *v = av[0];

    if (!strcmp(v, CMD_HELLO)) {
        InterlockedExchange(&g_connected, 1);
        send_snapshot();
    } else if (!strcmp(v, CMD_PLACE) && n >= 8) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) {
            int x = atoi(av[2]), y = atoi(av[3]);
            int ww = atoi(av[4]), hh = atoi(av[5]);
            w->rect.left = x; w->rect.top = y;
            w->rect.right = x + ww; w->rect.bottom = y + hh;
            w->ws = atoi(av[6]);
            w->z = atoi(av[7]);
            int bp = w->border_px * 2;
            win_set_client_size(w, ww - bp, hh - bp);
        }
    } else if (!strcmp(v, CMD_FOCUS) && n >= 2) {
        win_focus(win_find((unsigned)strtoul(av[1], NULL, 10)));
    } else if (!strcmp(v, CMD_WORKSPACE) && n >= 2) {
        g_srv.cur_ws = atoi(av[1]);
    } else if (!strcmp(v, CMD_BORDER) && n >= 4) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) {
            w->border_px = atoi(av[2]);
            unsigned long rgb = strtoul(av[3], NULL, 16);
            w->border_rgb = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
        }
    } else if (!strcmp(v, CMD_SPAWN)) {
        spawn_terminal(n >= 2 ? av[1] : NULL);   /* obs: sem tail; cmd sem espacos */
    } else if (!strcmp(v, CMD_CLOSE) && n >= 2) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) {
            unsigned id = w->id;
            win_destroy(w);
            wmproto_ev_destroyed(id);
        }
    } else if (!strcmp(v, CMD_GRAB) && n >= 3) {
        grab_add((unsigned)strtoul(av[1], NULL, 16), (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(v, CMD_UNGRAB) && n >= 3) {
        grab_del((unsigned)strtoul(av[1], NULL, 16), (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(v, CMD_QUIT)) {
        g_srv.running = 0;
    }
    /* FRAME-BEGIN / FRAME-COMMIT / TITLEBAR: aplicacao ja e' atomica por tick */
}

void wmproto_drain(void)
{
    char *line;
    while ((line = q_pop()) != NULL) {
        /* uma mensagem pode conter varias linhas — separa */
        for (char *s = strtok(line, "\r\n"); s; s = strtok(NULL, "\r\n")) {
            ntu_trim(s);
            if (*s)
                apply(s);
        }
        free(line);
        g_srv.dirty = 1;   /* comando aplicado -> recompoe neste tick */
    }
}

/* ---- thread leitora ---- */

static DWORD WINAPI reader_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        /* aceita um cliente (overlapped) */
        OVERLAPPED cov;
        ZeroMemory(&cov, sizeof cov);
        cov.hEvent = g_cev;
        ResetEvent(g_cev);
        BOOL con = ConnectNamedPipe(g_pipe, &cov);
        if (!con) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING)
                WaitForSingleObject(g_cev, INFINITE);
            else if (e != ERROR_PIPE_CONNECTED) {
                Sleep(100);
                continue;
            }
        }

        /* le mensagens ate desconectar */
        for (;;) {
            char buf[8192];
            DWORD nr = 0;
            OVERLAPPED rov;
            ZeroMemory(&rov, sizeof rov);
            rov.hEvent = g_rev;
            ResetEvent(g_rev);
            BOOL ok = ReadFile(g_pipe, buf, sizeof buf - 1, &nr, &rov);
            if (!ok && GetLastError() == ERROR_IO_PENDING)
                ok = GetOverlappedResult(g_pipe, &rov, &nr, TRUE);
            if (!ok || nr == 0)
                break;                 /* broken pipe: cliente saiu */
            buf[nr] = 0;
            q_push(buf);
        }

        InterlockedExchange(&g_connected, 0);
        DisconnectNamedPipe(g_pipe);   /* re-arma para o proximo ntwm */
    }
    return 0;
}

void wmproto_start(void)
{
    InitializeCriticalSection(&g_qlock);
    g_rev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_wev = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_cev = CreateEventA(NULL, TRUE, FALSE, NULL);

    g_pipe = CreateNamedPipeA(NTU_PIPE_DISPD,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
                              1, 65536, 65536, 0, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        dispd_log("wmproto: CreateNamedPipe falhou (%lu)", GetLastError());
        return;
    }
    g_reader = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
    dispd_log("wmproto: ouvindo em %s", NTU_PIPE_DISPD);
}
