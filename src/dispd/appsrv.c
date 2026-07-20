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
    DWORD  pid;               /* PID do processo do app (do named pipe, #52) */
    HANDLE rev, wev, ready;   /* eventos: read, write, ready(main confirmou) */
    CRITICAL_SECTION wlock;   /* serializa escritas do main/worker */
    volatile LONG dead;
    volatile LONG created;    /* 0=pendente, 1=ok, -1=falhou */
    volatile LONG commit;     /* audit #32: flag de commit pendente (nao dropa como a fila) */
    int proto_ver;
    AppRole role;
    unsigned anchors;
    int exclusive_zone;
    int interactivity;
    volatile LONG acked_serial;
    volatile LONG current_serial;
    volatile LONG announced;
    HANDLE pending_section;
    int pending_w, pending_h;
    char pending_name[80];
    struct AppConn *next;
} AppConn;

static AppConn *g_conns;
static CRITICAL_SECTION g_conns_lock;
static volatile LONG g_appctr;
static volatile LONG g_nconns;        /* conexoes vivas (quota anti-DoS #128) */
#define MAX_APPCONNS 32

typedef enum { AQ_CREATE, AQ_RECONFIGURE, AQ_DESTROY } AqType;
typedef struct {
    AqType   type;
    HANDLE   section;
    int      w, h;
    char     title[128];
    int      proto_ver;
    AppRole  role;
    unsigned anchors;
    int      exclusive_zone;
    int      interactivity;
    unsigned serial;
    AppConn *conn;
} AqItem;

#define AQCAP 256
static AqItem g_aq[AQCAP];
static int g_aqh, g_aqt;
static CRITICAL_SECTION g_aqlock;

static int aq_push(const AqItem *it)
{
    int ok = 0;
    EnterCriticalSection(&g_aqlock);
    int nt = (g_aqt + 1) % AQCAP;
    if (nt != g_aqh) { g_aq[g_aqt] = *it; g_aqt = nt; ok = 1; }
    LeaveCriticalSection(&g_aqlock);
    return ok;
}

/* teardown/create nao podem ser descartados (senao vaza section/conexao ou
 * deixa janela fantasma, #55/#56). O main drena a cada frame -> spin curto.
 * audit #34: mas NAO gira pra sempre se o main parou de drenar (travou/shutdown)
 * -> desiste apos ~5s e libera o recurso do item pra nao vazar. */
static void aq_push_reliable(const AqItem *it)
{
    int tries = 0;
    while (!aq_push(it)) {
        if (++tries > 5000) {
            if (it->type == AQ_CREATE && it->section)
                CloseHandle(it->section);
            return;
        }
        Sleep(1);
    }
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

/* escrita ao app (overlapped). wait_ms=quanto espera se a I/O ficar pendente;
 * kill_on_timeout=marca a conexao morta se estourar o tempo. audit #38: o
 * caminho de INPUT (main thread) chama com wait_ms=0 -> nunca bloqueia o
 * compositor (app lento => dropa o evento); o setup (worker) usa 1s+kill. */
static void app_send_to(AppConn *c, const char *line, DWORD wait_ms, int kill_on_timeout)
{
    if (!c)
        return;
    EnterCriticalSection(&c->wlock);
    /* re-checa dead/pipe SOB o lock: o teardown zera c->pipe sob o mesmo lock,
     * entao nunca escrevemos num handle ja fechado/reciclado */
    if (InterlockedCompareExchange(&c->dead, 0, 0) || !c->pipe) {
        LeaveCriticalSection(&c->wlock);
        return;
    }
    OVERLAPPED ov;
    ZeroMemory(&ov, sizeof ov);
    ov.hEvent = c->wev;
    ResetEvent(c->wev);
    DWORD w = 0;
    int timedout = 0;
    BOOL ok = WriteFile(c->pipe, line, (DWORD)strlen(line), &w, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(c->wev, wait_ms) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(c->pipe, &ov, &w, FALSE);
        else {
            /* espera o cancel terminar: 'ov' e 'line' sao locais e o kernel
             * ainda pode escrever neles ate a I/O cancelada completar (#60). */
            CancelIoEx(c->pipe, &ov);
            GetOverlappedResult(c->pipe, &ov, &w, TRUE);
            ok = FALSE;
            timedout = 1;
        }
    }
    LeaveCriticalSection(&c->wlock);
    /* falha REAL (pipe quebrado) sempre mata; timeout (app so lento) so mata se
     * pedido — assim um evento de input perdido nao derruba a app */
    if (!ok && (!timedout || kill_on_timeout))
        InterlockedExchange(&c->dead, 1);
}

static void app_send(AppConn *c, const char *line)   /* setup: bloqueia + mata */
{
    app_send_to(c, line, 1000, 1);
}

void appsrv_input_key(unsigned id, unsigned mods, unsigned vk, unsigned ch, int down)
{
    AppConn *c = conns_find(id);
    if (c) { char b[64]; snprintf(b, sizeof b, "APP-KEY %x %x %x %d", mods, vk, ch, down ? 1 : 0);
             app_send_to(c, b, 0, 0); }   /* #10 acao down/up; #38 nao bloqueia o compositor */
}

void appsrv_input_mouse(unsigned id, int x, int y, int buttons)
{
    AppConn *c = conns_find(id);
    if (c) { char b[64]; snprintf(b, sizeof b, "APP-MOUSE %d %d %d", x, y, buttons);
             app_send_to(c, b, 0, 0); }   /* #38: input real-time, zero-wait */
}

void appsrv_input_pointer(unsigned id, int x, int y, int buttons,
                          int button, int state, int axis)
{
    AppConn *c = conns_find(id);
    if (!c)
        return;
    char b[112];
    if (c->proto_ver >= 2) {
        snprintf(b, sizeof b, "%s %d %d %d %d %d %d", APP_EVT_POINTER,
                 x, y, buttons, button, state, axis);
        app_send_to(c, b, 0, 0);
    } else {
        appsrv_input_mouse(id, x, y, buttons);
    }
}

void appsrv_reconfigure_layers(void)
{
    /* Pode ser chamado antes de appsrv_start durante init/resize inicial. */
    if (!InterlockedCompareExchange(&g_nconns, 0, 0))
        return;
    EnterCriticalSection(&g_conns_lock);
    for (AppConn *c = g_conns; c; c = c->next) {
        if (c->proto_ver < 2 || c->role != APP_ROLE_LAYER ||
            InterlockedCompareExchange(&c->dead, 0, 0))
            continue;
        Window *w = win_find((unsigned)c->wid);
        if (!w)
            continue;
        int nw = w->cw, nh = w->ch;
        if ((c->anchors & (NTUAPP_ANCHOR_LEFT | NTUAPP_ANCHOR_RIGHT)) ==
            (NTUAPP_ANCHOR_LEFT | NTUAPP_ANCHOR_RIGHT))
            nw = g_srv.scr_w;
        if ((c->anchors & (NTUAPP_ANCHOR_TOP | NTUAPP_ANCHOR_BOTTOM)) ==
            (NTUAPP_ANCHOR_TOP | NTUAPP_ANCHOR_BOTTOM))
            nh = g_srv.scr_h;
        if (nw == w->cw && nh == w->ch)
            continue;
        EnterCriticalSection(&c->wlock);
        if (c->pending_section) {
            LeaveCriticalSection(&c->wlock);
            continue;
        }
        LONG serial = InterlockedIncrement(&c->current_serial);
        LONG id = InterlockedIncrement(&g_appctr);
        unsigned salt = (unsigned)GetTickCount() ^
                        ((unsigned)id * 2654435761u) ^ (unsigned)(UINT_PTR)c;
        snprintf(c->pending_name, sizeof c->pending_name,
                 "ntunix-appsurf-%ld-%08x", (long)id, salt);
        HANDLE section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                            PAGE_READWRITE, 0,
                                            (DWORD)(nw * nh * 4),
                                            c->pending_name);
        if (!section || GetLastError() == ERROR_ALREADY_EXISTS) {
            if (section) CloseHandle(section);
            LeaveCriticalSection(&c->wlock);
            continue;
        }
        c->pending_section = section;
        c->pending_w = nw;
        c->pending_h = nh;
        LeaveCriticalSection(&c->wlock);
        char msg[192];
        snprintf(msg, sizeof msg, "%s %ld %s %d %d", APP_EVT_CONFIGURE,
                 (long)serial, c->pending_name, nw, nh);
        app_send(c, msg);
    }
    LeaveCriticalSection(&g_conns_lock);
}

/* audit #39: mata o app por PID depois de um grace period, dando tempo pra ele
 * salvar apos o APP-CLOSE-REQUEST. Por PID (nao toca na conexao) -> sem corrida
 * com o worker; se o app ja saiu, o TerminateProcess num PID morto e' no-op. */
static DWORD WINAPI grace_kill(LPVOID arg)
{
    DWORD pid = (DWORD)(UINT_PTR)arg;
    Sleep(3000);
    HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
    if (h) { TerminateProcess(h, 0); CloseHandle(h); }
    return 0;
}

void appsrv_close_wid(unsigned id)
{
    AppConn *c = conns_find(id);
    if (!c)
        return;
    app_send(c, "APP-CLOSE-REQUEST");   /* pedido cooperativo (app pode salvar) */
    /* audit #39: NAO mata na hora — grace de 3s num thread; o app que sai antes
     * fecha o pipe (worker sai no EOF), o que trava vira kill no fim do grace. */
    if (c->pid) {
        HANDLE t = CreateThread(NULL, 0, grace_kill, (LPVOID)(UINT_PTR)c->pid, 0, NULL);
        if (t)
            CloseHandle(t);
        else {                           /* sem thread -> kill imediato (fallback) */
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, c->pid);
            if (h) { TerminateProcess(h, 0); CloseHandle(h); }
        }
    }
}

/* ---- main thread: aplica os pedidos ---- */

void appsrv_drain(void)
{
    int workarea_changed = 0;
    AqItem it;
    while (aq_pop(&it)) {
        if (it.type == AQ_CREATE) {
            Window *w = win_create_shared(it.w, it.h, it.section);
            if (w) {
                w->pid = it.conn->pid;   /* #52: janela conhece o processo dono */
                strncpy(w->title, it.title, sizeof w->title - 1);
                w->title[sizeof w->title - 1] = 0;
                for (char *p = w->title; *p; p++)          /* #17 sem injecao */
                    if ((unsigned char)*p < 0x20) *p = ' ';
                w->app_role = it.role;
                w->premultiplied = it.proto_ver >= 2;
                w->anchors = it.anchors;
                w->exclusive_zone = it.exclusive_zone;
                w->interactivity = it.interactivity;
                w->configure_serial = it.serial;
                if (it.role == APP_ROLE_LAYER) {
                    w->scene_layer = SCENE_LAYERS;
                    w->z = 10000;
                    w->border_px = 0;
                    w->titlebar = 0;
                    w->shadow = 0;
                    w->corner_radius = 0;
                    w->opacity = 255;
                    int x = (it.anchors & NTUAPP_ANCHOR_LEFT) ? 0 :
                            (it.anchors & NTUAPP_ANCHOR_RIGHT) ?
                            g_srv.scr_w - it.w : (g_srv.scr_w - it.w) / 2;
                    int y = (it.anchors & NTUAPP_ANCHOR_TOP) ? 0 :
                            (it.anchors & NTUAPP_ANCHOR_BOTTOM) ?
                            g_srv.scr_h - it.h : (g_srv.scr_h - it.h) / 2;
                    RECT goal = { x, y, x + it.w, y + it.h };
                    win_set_logical_rect(w, &goal);
                }
                w->visible = it.proto_ver < 2; /* v2 so mapeia no ACK+COMMIT */
                InterlockedExchange(&it.conn->wid, (LONG)w->id);
                InterlockedExchange(&it.conn->created, 1);
                if (it.proto_ver < 2) {
                    win_focus(w);
                    wmproto_ev_created(w);
                    InterlockedExchange(&it.conn->announced, 1);
                }
                dispd_log("app conectado (id %u, %dx%d, %s)", w->id, it.w, it.h, it.title);
            } else {
                if (it.section) CloseHandle(it.section);
                InterlockedExchange(&it.conn->created, -1);
            }
            SetEvent(it.conn->ready);   /* libera o worker p/ APP-SURFACE/ERR (#46) */
        } else if (it.type == AQ_RECONFIGURE) {
            Window *w = win_find((unsigned)it.conn->wid);
            if (!w || win_replace_shared(w, it.w, it.h, it.section) != 0) {
                if (it.section) CloseHandle(it.section);
            } else {
                w->configure_serial = it.serial;
                if (w->scene_layer == SCENE_LAYERS) {
                    int x = (w->anchors & NTUAPP_ANCHOR_LEFT) ? 0 :
                            (w->anchors & NTUAPP_ANCHOR_RIGHT) ?
                            g_srv.scr_w - it.w : (g_srv.scr_w - it.w) / 2;
                    int y = (w->anchors & NTUAPP_ANCHOR_TOP) ? 0 :
                            (w->anchors & NTUAPP_ANCHOR_BOTTOM) ?
                            g_srv.scr_h - it.h : (g_srv.scr_h - it.h) / 2;
                    RECT goal = { x, y, x + it.w, y + it.h };
                    win_set_logical_rect(w, &goal);
                }
            }
        } else { /* AQ_DESTROY: worker ja saiu e removeu do registro */
            unsigned id = (unsigned)it.conn->wid;
            Window *w = win_find(id);
            if (w) {
                int layer = w->scene_layer == SCENE_LAYERS;
                int announced = InterlockedCompareExchange(&it.conn->announced, 0, 0);
                win_destroy(w);
                if (!layer && announced)
                    wmproto_ev_destroyed(id);
                if (layer)
                    workarea_changed = 1;
            }
            if (it.conn->pending_section)
                CloseHandle(it.conn->pending_section);
            CloseHandle(it.conn->rev);
            CloseHandle(it.conn->wev);
            CloseHandle(it.conn->ready);
            DeleteCriticalSection(&it.conn->wlock);
            free(it.conn);
        }
    }

    /* audit #32: commits sao FLAG (nao fila) -> nunca sao dropados por fila cheia.
     * O main poll cada conexao aqui e marca a janela suja. */
    EnterCriticalSection(&g_conns_lock);
    for (AppConn *c = g_conns; c; c = c->next) {
        if (InterlockedExchange(&c->commit, 0)) {
            Window *w = win_find((unsigned)c->wid);
            if (w) {
                if (c->proto_ver >= 2 &&
                    InterlockedCompareExchange(&c->acked_serial, 0, 0) !=
                    InterlockedCompareExchange(&c->current_serial, 0, 0))
                    continue;
                if (!InterlockedCompareExchange(&c->announced, 0, 0)) {
                    w->visible = 1;
                    InterlockedExchange(&c->announced, 1);
                    if (w->scene_layer == SCENE_LAYERS)
                        workarea_changed = 1;
                    else {
                        win_focus(w);
                        wmproto_ev_created(w);
                    }
                }
                w->dirty = 1;
                g_srv.dirty = 1;
            }
        }
    }
    LeaveCriticalSection(&g_conns_lock);
    if (workarea_changed)
        compositor_recompute_workarea();
}

/* ---- worker por app (overlapped reads) ---- */

/* audit #35: verbo EXATO — o verbo tem que ser seguido de espaco ou fim, senao
 * "APP-COMMIT-xyz"/"APP-HELLOx" casavam por prefixo. */
static int verb_is(const char *buf, const char *verb)
{
    size_t n = strlen(verb);
    return !strncmp(buf, verb, n) && (buf[n] == 0 || buf[n] == ' ' || buf[n] == '\t');
}

static DWORD WINAPI worker_main(LPVOID arg)
{
    HANDLE pipe = (HANDLE)arg;
    if (InterlockedIncrement(&g_nconns) > MAX_APPCONNS) {   /* quota (#128) */
        InterlockedDecrement(&g_nconns);
        CloseHandle(pipe);
        return 0;
    }
    AppConn *c = (AppConn *)calloc(1, sizeof *c);
    if (!c) { InterlockedDecrement(&g_nconns); CloseHandle(pipe); return 0; }
    c->pipe = pipe;
    if (!GetNamedPipeClientProcessId(pipe, &c->pid) || c->pid == 0) {
        /* audit #40: sem PID confirmado nao da pra encerrar o processo ao fechar
         * a janela -> rejeita a conexao (identidade nao confirmada) */
        CloseHandle(pipe);
        free(c);
        InterlockedDecrement(&g_nconns);
        return 0;
    }
    {
        DWORD client_session = (DWORD)-1, server_session = (DWORD)-2;
        if (!ProcessIdToSessionId(c->pid, &client_session) ||
            !ProcessIdToSessionId(GetCurrentProcessId(), &server_session) ||
            client_session != server_session) {
            CloseHandle(pipe);
            free(c);
            InterlockedDecrement(&g_nconns);
            return 0;
        }
    }
    c->rev = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->wev = CreateEventA(NULL, TRUE, FALSE, NULL);
    c->ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!c->rev || !c->wev || !c->ready) {        /* #91: falha transacional */
        if (c->rev) CloseHandle(c->rev);
        if (c->wev) CloseHandle(c->wev);
        if (c->ready) CloseHandle(c->ready);
        CloseHandle(pipe);
        free(c);
        InterlockedDecrement(&g_nconns);
        return 0;
    }
    InitializeCriticalSection(&c->wlock);
    conns_add(c);

    int created = 0;
    char buf[512];
    ULONGLONG t0 = GetTickCount64();   /* audit #36: deadline ABSOLUTO de handshake */
    for (;;) {
        if (!created && GetTickCount64() - t0 > 8000)
            break;   /* nao completou o HELLO/CREATE a tempo (junk repetido) */
        DWORD n = 0;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = c->rev;
        ResetEvent(c->rev);
        BOOL ok = ReadFile(pipe, buf, sizeof buf - 1, &n, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            /* deadline de handshake: um cliente que conecta e nao manda HELLO
             * nao pode prender o worker pra sempre (#129) */
            DWORD to = created ? INFINITE : 8000;
            if (WaitForSingleObject(c->rev, to) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(pipe, &ov, &n, FALSE);
            else { CancelIoEx(pipe, &ov); GetOverlappedResult(pipe, &ov, &n, TRUE); ok = FALSE; }
        }
        int moredata = (!ok && GetLastError() == ERROR_MORE_DATA);
        if ((!ok && !moredata) || n == 0)
            break;
        buf[n] = 0;
        if (moredata) {
            /* audit #35: mensagem > buffer -> processa o pedaco truncado e DRENA
             * o resto (senao a cauda vira mensagem falsa no proximo read). Os
             * bytes ja estao no pipe (MORE_DATA), entao os reads completam rapido. */
            char sink[512];
            for (;;) {
                DWORD dn = 0;
                OVERLAPPED dov;
                ZeroMemory(&dov, sizeof dov);
                dov.hEvent = c->rev;
                ResetEvent(c->rev);
                BOOL dok = ReadFile(pipe, sink, sizeof sink, &dn, &dov);
                if (!dok && GetLastError() == ERROR_IO_PENDING) {
                    WaitForSingleObject(c->rev, INFINITE);
                    dok = GetOverlappedResult(pipe, &dov, &dn, FALSE);
                }
                if (dok || GetLastError() != ERROR_MORE_DATA)
                    break;
            }
        }

        if (verb_is(buf, "APP-HELLO") && !created) {
            char *p = buf + 9;
            while (*p == ' ' || *p == '\t') p++;
            int first = (int)strtol(p, &p, 10);
            int proto = 1;
            AppRole role = APP_ROLE_TOPLEVEL;
            unsigned anchors = 0;
            int exclusive = 0, interactivity = NTUAPP_INTERACT_ON_DEMAND;
            int w, h;
            if (first == NTUAPP_PROTO_VER) {
                proto = NTUAPP_PROTO_VER;
                while (*p == ' ' || *p == '\t') p++;
                char *role_start = p;
                while (*p && *p != ' ' && *p != '\t') p++;
                char saved = *p;
                *p = 0;
                if (!_stricmp(role_start, NTUAPP_ROLE_LAYER))
                    role = APP_ROLE_LAYER;
                else if (_stricmp(role_start, NTUAPP_ROLE_TOPLEVEL)) {
                    app_send(c, APP_EVT_ERROR " role-invalido");
                    break;
                }
                *p = saved;
                w = (int)strtol(p, &p, 10);
                h = (int)strtol(p, &p, 10);
                anchors = (unsigned)strtoul(p, &p, 0);
                exclusive = (int)strtol(p, &p, 10);
                interactivity = (int)strtol(p, &p, 10);
            } else {
                w = first;
                h = (int)strtol(p, &p, 10);
            }
            while (*p == ' ' || *p == '\t') p++;
            char title[128] = "app";
            if (*p) { strncpy(title, p, sizeof title - 1); title[sizeof title - 1] = 0; }
            ntu_trim(title);
            if (w < 1) w = 200;
            if (h < 1) h = 100;
            if (role == APP_ROLE_LAYER) {
                if ((anchors & (NTUAPP_ANCHOR_LEFT | NTUAPP_ANCHOR_RIGHT)) ==
                    (NTUAPP_ANCHOR_LEFT | NTUAPP_ANCHOR_RIGHT))
                    w = g_srv.scr_w;
                if ((anchors & (NTUAPP_ANCHOR_TOP | NTUAPP_ANCHOR_BOTTOM)) ==
                    (NTUAPP_ANCHOR_TOP | NTUAPP_ANCHOR_BOTTOM))
                    h = g_srv.scr_h;
                if (w > 4096) w = 4096;
                if (h > 1024) h = 1024;
                if (exclusive < 0) exclusive = 0;
                if (exclusive > h) exclusive = h;
                if (interactivity < NTUAPP_INTERACT_NONE ||
                    interactivity > NTUAPP_INTERACT_EXCLUSIVE)
                    interactivity = NTUAPP_INTERACT_NONE;
            }
            /* audit #37: teto por-surface baixado p/ 1024 (<=4MB/app) — 32 apps
             * x 4MB = 128MB = MemoryMax; antes 2048 dava 32x16MB = 512MB > limite.
             * (orcamento AGREGADO por bytes = refinamento futuro.) */
            if (role != APP_ROLE_LAYER && w > 1024) w = 1024;
            if (h > 1024) h = 1024;

            char name[80];
            LONG id = InterlockedIncrement(&g_appctr);
            unsigned salt = (unsigned)GetTickCount() ^ ((unsigned)id * 2654435761u) ^
                            (unsigned)(UINT_PTR)c;   /* nome menos previsivel (#130) */
            snprintf(name, sizeof name, "ntunix-appsurf-%ld-%08x", (long)id, salt);
            HANDLE sec = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL,
                                            PAGE_READWRITE, 0, (DWORD)(w * h * 4), name);
            if (!sec) { app_send(c, "APP-ERR no-surface"); break; }
            if (GetLastError() == ERROR_ALREADY_EXISTS) {   /* pre-criado por outro (#131) */
                CloseHandle(sec);
                app_send(c, "APP-ERR name-collision");
                break;
            }

            /* enfileira CREATE e ESPERA o main confirmar antes de dar a surface (#46) */
            AqItem it;
            ZeroMemory(&it, sizeof it);
            it.type = AQ_CREATE; it.section = sec; it.w = w; it.h = h;
            strcpy(it.title, title);
            it.proto_ver = proto; it.role = role; it.anchors = anchors;
            it.exclusive_zone = exclusive; it.interactivity = interactivity;
            it.serial = 1;
            it.conn = c;
            c->proto_ver = proto; c->role = role; c->anchors = anchors;
            c->exclusive_zone = exclusive; c->interactivity = interactivity;
            InterlockedExchange(&c->current_serial, 1);
            ResetEvent(c->ready);
            aq_push_reliable(&it);   /* nao perde o CREATE (senao vaza a section, #56) */
            WaitForSingleObject(c->ready, 5000);

            if (InterlockedCompareExchange(&c->created, 0, 0) == 1) {
                char resp[192];
                if (proto >= 2) {
                    snprintf(resp, sizeof resp, "%s %d %ld", APP_EVT_WELCOME,
                             NTUAPP_PROTO_VER, (long)c->wid);
                    app_send(c, resp);
                    snprintf(resp, sizeof resp, "%s 1 %s %d %d",
                             APP_EVT_CONFIGURE, name, w, h);
                    app_send(c, resp);
                } else {
                    snprintf(resp, sizeof resp, "%s %s %d %d",
                             APP_EVT_SURFACE, name, w, h);
                    app_send(c, resp);
                }
                created = 1;
            } else {
                app_send(c, "APP-ERR create-failed");
                break;
            }
        } else if (verb_is(buf, APP_CMD_ACK) && c->proto_ver >= 2) {
            unsigned serial = (unsigned)strtoul(buf + strlen(APP_CMD_ACK), NULL, 10);
            if (serial != (unsigned)InterlockedCompareExchange(&c->current_serial, 0, 0))
                continue;
            InterlockedExchange(&c->acked_serial, (LONG)serial);
            EnterCriticalSection(&c->wlock);
            HANDLE pending = c->pending_section;
            if (pending) {
                AqItem it;
                ZeroMemory(&it, sizeof it);
                it.type = AQ_RECONFIGURE;
                it.section = pending;
                it.w = c->pending_w;
                it.h = c->pending_h;
                it.serial = serial;
                it.conn = c;
                c->pending_section = NULL;
                LeaveCriticalSection(&c->wlock);
                aq_push_reliable(&it);
            } else {
                LeaveCriticalSection(&c->wlock);
            }
        } else if (verb_is(buf, APP_CMD_COMMIT)) {
            unsigned serial = c->proto_ver >= 2
                ? (unsigned)strtoul(buf + strlen(APP_CMD_COMMIT), NULL, 10) : 0;
            if (c->proto_ver < 2 ||
                (serial != 0 &&
                 serial == (unsigned)InterlockedCompareExchange(&c->acked_serial, 0, 0)))
                InterlockedExchange(&c->commit, 1);
        } else if (verb_is(buf, "APP-CLOSE")) {
            break;
        }
    }

    /* zera c->pipe SOB o wlock antes de fechar, pra nao correr com app_send */
    EnterCriticalSection(&c->wlock);
    InterlockedExchange(&c->dead, 1);
    c->pipe = NULL;
    LeaveCriticalSection(&c->wlock);
    conns_remove(c);
    CloseHandle(pipe);
    InterlockedDecrement(&g_nconns);   /* libera a quota (#128) */

    AqItem it;
    ZeroMemory(&it, sizeof it);
    it.type = AQ_DESTROY; it.conn = c;
    aq_push_reliable(&it);   /* teardown nao pode ser descartado (#55) */
    return 0;
}

static DWORD WINAPI accept_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        HANDLE pipe = CreateNamedPipeA(NTU_PIPE_DISPD_APP,
                                       PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                       PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE |
                                       PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,  /* audit #118 */
                                       PIPE_UNLIMITED_INSTANCES,
                                       65536, 65536, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) { Sleep(500); continue; }

        HANDLE cev = CreateEventA(NULL, TRUE, FALSE, NULL);
        OVERLAPPED cov;
        ZeroMemory(&cov, sizeof cov);
        cov.hEvent = cev;
        BOOL con = ConnectNamedPipe(pipe, &cov)
                       ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!con && GetLastError() == ERROR_IO_PENDING) {
            /* audit #41: valida a conclusao do connect (nao assume con=TRUE) */
            if (WaitForSingleObject(cev, INFINITE) == WAIT_OBJECT_0) {
                DWORD dummy;
                con = GetOverlappedResult(pipe, &cov, &dummy, FALSE);
            } else {
                CancelIoEx(pipe, &cov);
                con = FALSE;
            }
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
    if (t) {                          /* audit #42: so loga 'ouvindo' se a thread subiu */
        CloseHandle(t);
        dispd_log("appsrv: ouvindo em %s", NTU_PIPE_DISPD_APP);
    } else {
        dispd_log("appsrv: CreateThread(accept) falhou (%lu) — apps indisponiveis",
                  GetLastError());
    }
}
