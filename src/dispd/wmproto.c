/*
 * wmproto.c — servidor do protocolo dispd <-> ntwm (NTU_PIPE_DISPD).
 *
 * Canal duplex persistente, message-mode, OVERLAPPED. Uma thread leitora
 * enfileira os comandos do ntwm; o main thread do dispd drena a fila (aplica)
 * e escreve os eventos. Um unico escritor (main thread) => sem lock de escrita.
 *
 * TRANSACAO ATOMICA: os comandos entre FRAME-BEGIN e FRAME-COMMIT sao
 * BUFFERIZADOS e aplicados de uma vez no COMMIT (swap). O estado vivo (usado
 * por input/hit-test/compose) so transiciona do quadro antigo pro novo no
 * COMMIT — nunca observa um quadro parcial (#21/#29). Um frame que nao commita
 * (WM travado) e' DESCARTADO, nao publicado pela metade (#30).
 *
 * GERACAO: cada conexao tem um numero; comandos de uma conexao morta (geracao
 * antiga) sao descartados no drain, e um HELLO obsoleto nao marca conectado
 * (#66/#67). Overflow da fila -> pede resync ao WM (#71).
 *
 * Estado de janela e' todo do dispd; se o ntwm cai, o desktop continua e um
 * ntwm novo recebe um snapshot (WINDOW... + SYNC) e re-declara o layout.
 */
#include "dispd.h"
#include "../common/ntuwm.h"

#define QCAP 512
#define GRABCAP 64
#define FRAMECAP 512
#define DRAIN_BUDGET 2048    /* comandos por tick: evita starvation (#72) */

static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static HANDLE g_reader;
static HANDLE g_rev, g_wev, g_cev;     /* eventos: read, write, connect */
static OVERLAPPED g_wov;               /* overlapped do escritor (main thread) */
static volatile LONG g_connected;
static volatile LONG g_need_reset;     /* reader sinaliza desconexao -> main reseta */
static volatile LONG g_overflow;       /* fila estourou -> pedir resync (#71) */
static volatile LONG g_gen;            /* geracao da conexao atual (#66/#67) */

/* fila de comandos, com geracao (produtor: reader; consumidor: main) */
typedef struct { char *s; LONG gen; } QItem;
static QItem g_q[QCAP];
static int g_qh, g_qt;
static CRITICAL_SECTION g_qlock;

/* tabela de grabs (so tocada pelo main thread) */
static struct { unsigned mods, vk; } g_grabs[GRABCAP];
static int g_ngrabs;

/* buffer da transacao de layout (main thread) */
static char *g_frame[FRAMECAP];
static int g_nframe;
static int g_frame_failed;   /* quadro perdeu comando (overflow/OOM) -> nao commita (#44,#45) */
static int g_buffering;

static int g_hello;                    /* ja recebeu HELLO valido? (main thread) */

/* ---- fila ---- */

static int q_push(const char *s, LONG gen)
{
    char *dup = _strdup(s);
    if (!dup) {
        InterlockedExchange(&g_overflow, 1);   /* audit #57: OOM tambem pede RESYNC */
        return 0;
    }
    EnterCriticalSection(&g_qlock);
    int nt = (g_qt + 1) % QCAP;
    if (nt == g_qh) {           /* cheia: NAO descarta silenciosamente (#71) */
        LeaveCriticalSection(&g_qlock);
        free(dup);
        InterlockedExchange(&g_overflow, 1);
        return 0;
    }
    g_q[g_qt].s = dup;
    g_q[g_qt].gen = gen;
    g_qt = nt;
    LeaveCriticalSection(&g_qlock);
    return 1;
}

static char *q_pop(LONG *gen)
{
    char *s = NULL;
    EnterCriticalSection(&g_qlock);
    if (g_qh != g_qt) {
        s = g_q[g_qh].s;
        *gen = g_q[g_qh].gen;
        g_qh = (g_qh + 1) % QCAP;
    }
    LeaveCriticalSection(&g_qlock);
    return s;
}

/* ---- escrita de eventos (main thread) ---- */

/* escreve no pipe SEM checar g_connected — usado por respostas de handshake
 * (ex.: erro de versao antes de conectar de fato — audit #54) */
static void wm_write(const char *line)
{
    ResetEvent(g_wev);
    ZeroMemory(&g_wov, sizeof g_wov);
    g_wov.hEvent = g_wev;
    DWORD w = 0;
    BOOL ok = WriteFile(g_pipe, line, (DWORD)strlen(line), &w, &g_wov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        /* timeout: ntwm travado nao pode congelar o compositor (#14) */
        if (WaitForSingleObject(g_wev, 2000) == WAIT_OBJECT_0)
            ok = GetOverlappedResult(g_pipe, &g_wov, &w, FALSE);
        else {
            /* espera o cancelamento TERMINAR antes de reusar g_wov (#74) */
            CancelIoEx(g_pipe, &g_wov);
            GetOverlappedResult(g_pipe, &g_wov, &w, TRUE);
            ok = FALSE;
        }
    }
    if (!ok) {
        InterlockedExchange(&g_connected, 0);
        InterlockedExchange(&g_need_reset, 1);
    }
}

/* eventos normais dispd->ntwm: so quando ha um WM conectado */
static void wm_send(const char *line)
{
    if (InterlockedCompareExchange(&g_connected, 0, 0))
        wm_write(line);
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

void wmproto_ev_focused(Window *w)
{
    char b[64];
    snprintf(b, sizeof b, "%s %u", EVT_FOCUSED, w ? w->id : 0);
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

/* audit #85: a tela mudou de tamanho -> reenvia OUTPUT + SYNC pro WM re-tilar */
void wmproto_ev_output(void)
{
    char b[128];
    snprintf(b, sizeof b, "%s 0 0 %d %d %d", EVT_OUTPUT, g_srv.bar_h,
             g_srv.scr_w, g_srv.scr_h - g_srv.bar_h);
    wm_send(b);
    wm_send(EVT_SYNC);
}

/* audit #3: clique num workspace da barra -> pede ao WM pra trocar (ele e' dono
 * da politica de layout; nao trocamos cur_ws unilateralmente). */
void wmproto_ev_wsreq(int ws)
{
    char b[32];
    snprintf(b, sizeof b, "%s %d", EVT_WSREQ, ws);
    wm_send(b);
}

/* audit #5: o usuario arrastou uma janela floating -> o WM guarda a geometria e
 * passa a declara-la (em vez da cascata) nos proximos frames. */
void wmproto_ev_moved(unsigned id, int x, int y, int w, int h)
{
    char b[64];
    snprintf(b, sizeof b, "%s %u %d %d %d %d", EVT_MOVED, id, x, y, w, h);
    wm_send(b);
}

static void send_snapshot(void)
{
    char b[512];
    snprintf(b, sizeof b, "%s dispd %d %s", EVT_WELCOME, NTUWM_PROTO_VER, ntu_root());
    wm_send(b);
    snprintf(b, sizeof b, "%s 0 0 %d %d %d", EVT_OUTPUT, g_srv.bar_h,
             g_srv.scr_w, g_srv.scr_h - g_srv.bar_h);
    wm_send(b);
    snprintf(b, sizeof b, "%s %d", EVT_CURWS, g_srv.cur_ws);
    wm_send(b);

    /* ordem estavel: a lista do dispd e' prepend (mais novo primeiro); envio em
     * ordem REVERSA (mais antigo primeiro) pra que o ntwm, que tambem prepend,
     * reconstrua a mesma ordem master/stack (#32). Inclui ws e floating (#33). */
    Window *arr[256];
    int nw = 0;
    for (Window *w = g_srv.windows; w && nw < 256; w = w->next)
        arr[nw++] = w;
    for (int i = nw - 1; i >= 0; i--) {
        Window *w = arr[i];
        snprintf(b, sizeof b, "%s %u %d %lu %d %d %s", EVT_WINDOW, w->id,
                 (int)w->kind, w->pid, w->ws, w->floating,
                 w->title[0] ? w->title : (w->kind == WK_TERM ? "terminal" : "app"));
        wm_send(b);
    }
    if (g_srv.focused)
        snprintf(b, sizeof b, "%s %u", EVT_FOCUSED, g_srv.focused->id);
    else
        snprintf(b, sizeof b, "%s 0", EVT_FOCUSED);   /* sempre manda foco (#19) */
    wm_send(b);
    wm_send(EVT_SYNC);
}

/* ---- aplicacao efetiva de um comando (main thread) ---- */

static void apply_now(char *line)
{
    /* SPAWN-TERM: cmdline pode ter espacos -> trata antes do split destrutivo */
    size_t sl = strlen(CMD_SPAWN);
    if (!strncmp(line, CMD_SPAWN, sl) && (line[sl] == 0 || line[sl] == ' ')) {
        const char *p = line + sl;
        while (*p == ' ' || *p == '\t') p++;
        spawn_terminal(*p ? p : NULL);
        return;
    }

    char *av[10];
    int n = ntuwm_split(line, av, 10, -1);
    if (n < 1)
        return;
    const char *v = av[0];

    if (!strcmp(v, CMD_PLACE) && n >= 8) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) {
            /* parse largo + clamp: origem/tamanho de um WM malformado nao podem
             * estourar o RECT (int) na soma x+ww (#42). */
            long x = strtol(av[2], NULL, 10), y = strtol(av[3], NULL, 10);
            long ww = strtol(av[4], NULL, 10), hh = strtol(av[5], NULL, 10);
            long xlim = (long)g_srv.scr_w * 4, ylim = (long)g_srv.scr_h * 4;
            if (ww < 1) ww = 1;
            if (hh < 1) hh = 1;
            if (ww > xlim) ww = xlim;
            if (hh > ylim) hh = ylim;
            if (x < -xlim) x = -xlim; else if (x > xlim) x = xlim;
            if (y < -ylim) y = -ylim; else if (y > ylim) y = ylim;
            int ws = atoi(av[6]);
            if (ws < 0 || ws >= NTUWM_WS) ws = w->ws;               /* valida ws (#43) */
            w->rect.left = (int)x; w->rect.top = (int)y;
            w->rect.right = (int)(x + ww); w->rect.bottom = (int)(y + hh);
            w->ws = ws;
            w->z = atoi(av[7]);
            int b = w->border_px;
            if (w->kind == WK_FOREIGN)      /* posiciona o HWND real, inset na chrome */
                foreign_place(w, (int)x + b, (int)y + b + g_srv.title_h,
                              (int)ww - 2 * b, (int)hh - 2 * b - g_srv.title_h);
            else
                win_set_client_size(w, (int)ww - 2 * b, (int)hh - 2 * b - g_srv.title_h);
        }
    } else if (!strcmp(v, CMD_FOCUS) && n >= 2) {
        win_focus(win_find((unsigned)strtoul(av[1], NULL, 10)));  /* 0 -> NULL */
    } else if (!strcmp(v, CMD_WORKSPACE) && n >= 2) {
        int ws = atoi(av[1]);
        if (ws >= 0 && ws < NTUWM_WS)
            g_srv.cur_ws = ws;
    } else if (!strcmp(v, CMD_BORDER) && n >= 4) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) {
            int px = atoi(av[2]);
            w->border_px = (px < 0) ? 0 : (px > 32 ? 32 : px);
            unsigned long rgb = strtoul(av[3], NULL, 16);
            w->border_rgb = RGB((rgb >> 16) & 0xff, (rgb >> 8) & 0xff, rgb & 0xff);
        }
    } else if (!strcmp(v, CMD_SETWS) && n >= 3) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        int ws = atoi(av[2]);                       /* muda ws sem redimensionar (#28) */
        if (w && ws >= 0 && ws < NTUWM_WS) w->ws = ws;
    } else if (!strcmp(v, CMD_FLOAT) && n >= 3) {
        Window *w = win_find((unsigned)strtoul(av[1], NULL, 10));
        if (w) w->floating = atoi(av[2]) ? 1 : 0;   /* persiste p/ restart (#33) */
    } else if (!strcmp(v, CMD_CLOSE) && n >= 2) {
        dispd_close_window(win_find((unsigned)strtoul(av[1], NULL, 10)));   /* #3/#54 */
    } else if (!strcmp(v, CMD_GRAB) && n >= 3) {
        grab_add((unsigned)strtoul(av[1], NULL, 16), (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(v, CMD_UNGRAB) && n >= 3) {
        grab_del((unsigned)strtoul(av[1], NULL, 16), (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(v, CMD_QUIT)) {
        g_srv.running = 0;
    }
    /* CMD_TITLEBAR: dispd ja desenha titulos sempre — aceito sem efeito extra */
}

/* ---- transacao de layout ---- */

static void frame_clear(void)
{
    for (int i = 0; i < g_nframe; i++)
        free(g_frame[i]);
    g_nframe = 0;
    g_frame_failed = 0;
}

static void frame_push(const char *line)
{
    if (g_nframe >= FRAMECAP) {          /* quadro grande demais: invalida tudo */
        g_frame_failed = 1;              /* #44: nao publica os 512 parciais */
        InterlockedExchange(&g_overflow, 1);
        return;
    }
    char *dup = _strdup(line);
    if (!dup) {
        g_frame_failed = 1;              /* #45: OOM invalida o quadro inteiro */
        InterlockedExchange(&g_overflow, 1);
        return;
    }
    g_frame[g_nframe++] = dup;
}

static void frame_commit(void)
{
    /* audit #44 (critico)/#45: um quadro que perdeu QUALQUER comando (overflow
     * do FRAMECAP ou OOM no _strdup) NAO pode ser publicado pela metade —
     * corromperia o estado global de layout. Descarta tudo e pede RESYNC. */
    if (g_frame_failed) {
        dispd_log("wmproto: quadro incompleto (overflow/OOM) descartado — RESYNC");
        frame_clear();
        InterlockedExchange(&g_overflow, 1);   /* forca RESYNC no proximo drain */
        return;
    }
    for (int i = 0; i < g_nframe; i++)
        apply_now(g_frame[i]);   /* swap atomico: aplica o quadro inteiro de uma vez */
    frame_clear();
}

void wmproto_abort_frame(void)   /* frame travado: DESCARTA, nao publica parcial (#30) */
{
    frame_clear();
    g_buffering = 0;
    g_srv.in_frame = 0;
    g_srv.dirty = 1;             /* garante repaint apos abortar (#31) */
}

static int is_frame_verb(const char *v)
{
    return !strcmp(v, CMD_PLACE) || !strcmp(v, CMD_FOCUS) ||
           !strcmp(v, CMD_WORKSPACE) || !strcmp(v, CMD_SETWS) ||
           !strcmp(v, CMD_BORDER) || !strcmp(v, CMD_FLOAT) ||
           !strcmp(v, CMD_TITLEBAR);
}

/* dispatcher: controla a transacao e decide bufferizar ou aplicar direto */
static void apply(char *line)
{
    /* extrai o verbo sem destruir a linha (frame_push guarda uma copia) */
    char verb[24];
    const char *sp = strchr(line, ' ');
    int vl = sp ? (int)(sp - line) : (int)strlen(line);
    if (vl >= (int)sizeof verb) vl = (int)sizeof verb - 1;
    memcpy(verb, line, (size_t)vl);
    verb[vl] = 0;

    if (!strcmp(verb, CMD_HELLO)) {                 /* handshake */
        char *av[4];
        int n = ntuwm_split(line, av, 4, -1);
        int ver = n >= 3 ? atoi(av[2]) : 0;
        if (ver != NTUWM_PROTO_VER) {               /* valida versao (#65) */
            wm_write(EVT_ERR " versao-incompativel");   /* audit #54: ERR mesmo sem conectar */
            dispd_log("wmproto: HELLO com versao %d != %d — rejeitado", ver, NTUWM_PROTO_VER);
            return;
        }
        if (g_hello) {                              /* segundo HELLO e' erro (#68) */
            wm_write(EVT_ERR " hello-duplicado");   /* audit #54 */
            return;
        }
        g_ngrabs = 0;
        g_hello = 1;
        frame_clear();
        g_buffering = 0;
        g_srv.in_frame = 0;
        InterlockedExchange(&g_connected, 1);
        send_snapshot();
        return;
    }
    if (!g_hello)
        return;                                     /* ignora comandos antes do HELLO */

    if (!strcmp(verb, CMD_PONG))                    /* heartbeat: recebe-lo ja basta (#62) */
        return;

    if (!strcmp(verb, CMD_FRAME_BEGIN)) {
        frame_clear();
        g_buffering = 1;
        g_srv.in_frame = 1;                          /* nao apresenta ate COMMIT */
        return;
    }
    if (!strcmp(verb, CMD_FRAME_COMMIT)) {
        if (!g_buffering) {                          /* audit #55: COMMIT sem BEGIN -> ignora
                                                      * (nao publica um quadro que nunca abriu) */
            dispd_log("wmproto: COMMIT sem BEGIN — ignorado");
            return;
        }
        frame_commit();                             /* swap atomico */
        g_buffering = 0;
        g_srv.in_frame = 0;
        g_srv.dirty = 1;
        return;
    }
    if (g_buffering && is_frame_verb(verb)) {
        frame_push(line);                           /* acumula o quadro */
        return;
    }
    apply_now(line);                                /* fora de transacao: imediato */
}

/* reseta estado do WM apos desconexao (main thread) — grabs, hello, frame, fila */
static void reset_wm_state(void)
{
    g_ngrabs = 0;
    g_hello = 0;
    frame_clear();
    g_buffering = 0;
    g_srv.in_frame = 0;
    /* audit #47: NAO limpa a fila aqui — isso apagava o HELLO da conexao NOVA.
     * O bump de g_gen na desconexao ja invalida os comandos do WM morto (#66):
     * o check `gen != cur` no drain os descarta ao popar. */
    InterlockedExchange(&g_overflow, 0);
    g_srv.dirty = 1;
}

void wmproto_drain(void)
{
    if (InterlockedExchange(&g_need_reset, 0))
        reset_wm_state();

    /* overflow: perdemos comandos -> descarta o quadro parcial e pede ao WM que
     * re-declare tudo (resync), evitando estado corrompido (#71) */
    if (InterlockedExchange(&g_overflow, 0)) {
        frame_clear();
        g_buffering = 0;
        g_srv.in_frame = 0;
        /* audit #46: bump da geracao ANTES do RESYNC — os comandos ja enfileirados
         * (o frame stale declarado contra o snapshot antigo) ficam com gen != cur
         * e o drain os descarta; so os comandos POS-RESYNC (nova gen) sao aplicados.
         * Reusa o mesmo mecanismo do #47/#66 em vez de um serial de protocolo. */
        InterlockedIncrement(&g_gen);
        dispd_log("wmproto: fila estourou — pedindo RESYNC ao ntwm");
        wm_send(EVT_RESYNC);
    }

    /* heartbeat (#62): PING periodico (~2s) — o WM sadio responde PONG e mantem
     * a leitora viva; um WM travado nao responde e estoura o deadline de 6s. */
    static unsigned ping_tick;
    if (g_hello && (ping_tick++ % 120) == 0)
        wm_send(EVT_PING);

    LONG cur = InterlockedCompareExchange(&g_gen, 0, 0);
    char *line;
    LONG gen;
    int budget = DRAIN_BUDGET;
    while (budget-- > 0 && (line = q_pop(&gen)) != NULL) {
        if (gen != cur) { free(line); continue; }   /* comando de conexao morta (#67) */
        for (char *s = strtok(line, "\r\n"); s; s = strtok(NULL, "\r\n")) {
            ntu_trim(s);
            if (*s)
                apply(s);
        }
        free(line);
        if (!g_srv.in_frame)
            g_srv.dirty = 1;   /* fora de transacao: recompoe neste tick */
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
            if (e == ERROR_IO_PENDING) {
                WaitForSingleObject(g_cev, INFINITE);
                DWORD dummy;
                if (!GetOverlappedResult(g_pipe, &cov, &dummy, FALSE)) {   /* #82 */
                    DisconnectNamedPipe(g_pipe);
                    continue;
                }
            } else if (e != ERROR_PIPE_CONNECTED) {
                Sleep(100);
                continue;
            }
        }

        LONG cur = InterlockedIncrement(&g_gen);   /* nova geracao de conexao */

        /* le mensagens (remontando fragmentos ERROR_MORE_DATA, #58/#78) */
        enum { ACCCAP = 65536 };
        char *acc = (char *)malloc(ACCCAP);
        if (!acc) { DisconnectNamedPipe(g_pipe); Sleep(100); continue; }
        int got_first = 0;
        for (;;) {
            size_t acclen = 0;
            int broken = 0, complete = 0;
            while (!complete) {
                char buf[8192];
                DWORD nr = 0;
                OVERLAPPED rov;
                ZeroMemory(&rov, sizeof rov);
                rov.hEvent = g_rev;
                ResetEvent(g_rev);
                BOOL ok = ReadFile(g_pipe, buf, sizeof buf, &nr, &rov);
                if (!ok && GetLastError() == ERROR_IO_PENDING) {
                    /* deadline: cliente que conecta e nao fala nao pode prender o
                     * slot pra sempre (#84). audit #49/#62: mesmo APOS o handshake
                     * ha deadline (heartbeat) — o dispd manda PING periodico e o
                     * WM sadio responde PONG; um WM travado (vivo mas mudo) nao
                     * responde, estoura o deadline e cai (initd respawna). */
                    DWORD to = got_first ? 6000 : 8000;
                    if (WaitForSingleObject(g_rev, to) == WAIT_OBJECT_0)
                        ok = GetOverlappedResult(g_pipe, &rov, &nr, FALSE);
                    else {
                        CancelIoEx(g_pipe, &rov);
                        GetOverlappedResult(g_pipe, &rov, &nr, TRUE);
                        ok = FALSE;
                    }
                }
                DWORD err = ok ? 0 : GetLastError();
                if (!ok && err != ERROR_MORE_DATA) { broken = 1; break; }
                if (nr == 0 && ok) { broken = 1; break; }
                if (acclen + nr < (size_t)ACCCAP) {
                    memcpy(acc + acclen, buf, nr);
                    acclen += nr;
                } /* > 64KB: trunca (mantem a conexao viva, #78) */
                if (err != ERROR_MORE_DATA)
                    complete = 1;
            }
            if (broken)
                break;
            if (acclen > 0) {
                acc[acclen] = 0;
                q_push(acc, cur);
                got_first = 1;   /* handshake iniciado: reads seguintes sem deadline */
            }
        }
        free(acc);

        InterlockedExchange(&g_connected, 0);
        InterlockedIncrement(&g_gen);            /* audit #47/#66: invalida os comandos
                                                  * da conexao morta (o check de gen no
                                                  * drain os descarta) — assim nao precisa
                                                  * limpar a fila e apagar o HELLO da nova */
        InterlockedExchange(&g_need_reset, 1);   /* main reseta grabs/hello */
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
    if (!g_rev || !g_wev || !g_cev) {              /* #81 */
        dispd_log("wmproto: CreateEvent falhou (%lu)", GetLastError());
        return;
    }

    g_pipe = CreateNamedPipeA(NTU_PIPE_DISPD,
                              PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                              PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
                              PIPE_REJECT_REMOTE_CLIENTS,   /* audit #116: sem clientes de rede */
                              1, 65536, 65536, 0, NULL);
    if (g_pipe == INVALID_HANDLE_VALUE) {
        dispd_log("wmproto: CreateNamedPipe falhou (%lu)", GetLastError());
        return;
    }
    g_reader = CreateThread(NULL, 0, reader_main, NULL, 0, NULL);
    if (!g_reader) {                               /* #81 */
        dispd_log("wmproto: CreateThread(reader) falhou (%lu)", GetLastError());
        CloseHandle(g_pipe);
        g_pipe = INVALID_HANDLE_VALUE;
        return;
    }
    dispd_log("wmproto: ouvindo em %s", NTU_PIPE_DISPD);
}

/* audit #50: teardown do servidor WM no shutdown — cancela a I/O da leitora,
 * fecha o pipe e JUNTA a thread (com timeout). Antes so contava com o exit. */
void wmproto_stop(void)
{
    HANDLE p = g_pipe;
    if (p != INVALID_HANDLE_VALUE) {
        g_pipe = INVALID_HANDLE_VALUE;
        CancelIoEx(p, NULL);        /* desbloqueia o ReadFile overlapped da leitora */
        DisconnectNamedPipe(p);
        CloseHandle(p);
    }
    if (g_reader) {
        if (WaitForSingleObject(g_reader, 2000) != WAIT_OBJECT_0)
            dispd_log("wmproto: leitora nao encerrou em 2s no shutdown");
        CloseHandle(g_reader);
        g_reader = NULL;
    }
    dispd_log("wmproto: servidor encerrado");
}
