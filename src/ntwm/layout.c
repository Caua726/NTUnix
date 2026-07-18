/*
 * layout.c — modelo de clientes + tiling, portado do dwm.
 *
 *   tile()      -> dwm tile(): coluna master (ww*mfact) + coluna stack,
 *                  empilhamento vertical, altura = restante/(linhas restantes).
 *   focusstack  -> dwm focusstack (cicla foco entre visiveis).
 *   setmfact    -> clamp [0.05, 0.95]; incnmaster -> piso 0.
 *   view/tagto  -> workspaces (nosso ws inteiro por janela, nao bitmask).
 *   zoom        -> promove o focado a master (dwm pop/attach).
 *
 * O ntwm declara o quadro inteiro entre FRAME-BEGIN/FRAME-COMMIT; o dispd
 * aplica atomico (sem as corridas do X11).
 */
#include "ntwm.h"

Client *g_clients;
Client *g_focused;
int     g_curws;
int     g_nmaster = 1;
float   g_mfact = 0.55f;
int     g_wx, g_wy, g_ww, g_wh;
int     g_gap = 8;
int     g_border = 2;

Client *cl_find(unsigned id)
{
    for (Client *c = g_clients; c; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

Client *cl_add(unsigned id, int ws, int floating)
{
    if (cl_find(id))
        return NULL;
    Client *c = (Client *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->id = id;
    c->ws = ws;
    c->floating = floating;   /* restaura floating do snapshot (#33) */
    strncpy(c->title, "terminal", sizeof c->title - 1);
    c->next = g_clients;    /* prepend: novo vira master (dwm attach) */
    g_clients = c;
    g_focused = c;
    wm_send("%s %u %d %06x", CMD_BORDER, id, g_border, 0x40404a);
    return c;
}

void cl_remove(unsigned id)
{
    Client **pp = &g_clients;
    while (*pp && (*pp)->id != id)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    Client *c = *pp;
    *pp = c->next;
    if (g_focused == c) {
        g_focused = NULL;
        for (Client *o = g_clients; o; o = o->next)
            if (o->ws == g_curws) { g_focused = o; break; }
        if (!g_focused)
            g_focused = g_clients;
    }
    free(c);
}

/* janela movida de workspace por tagto(): comunicada ao dispd DENTRO do frame
 * (senao um PLACE 1x1 isolado, se perdido, deixa dispd e ntwm divergentes #28) */
static unsigned g_move_id;
static int      g_move_ws;
static int      g_move_pending;

/* dwm tile(): master-stack em duas colunas, com gaps uniformes. */
void send_frame(void)
{
    wm_send(CMD_FRAME_BEGIN);
    wm_send("%s %d", CMD_WORKSPACE, g_curws);   /* ws atomico com o layout (#27) */
    if (g_move_pending) {                        /* move de ws dentro do frame (#28) */
        wm_send("%s %u %d", CMD_SETWS, g_move_id, g_move_ws);
        g_move_pending = 0;
    }

    /* audit #26: o estado floating de cada janela vai DENTRO do frame (atomico
     * com o layout), em vez de num CMD_FLOAT solto antes do send_frame */
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == g_curws)
            wm_send("%s %u %d", CMD_FLOAT, c->id, c->floating);

    /* conta so os tiled (nao-floating) do ws atual */
    int n = 0;
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == g_curws && !c->floating)
            n++;

    int g = g_gap;
    /* nao deixa os gaps consumirem toda a area: cada janela precisa de >=1px.
     * audit #22: clampa por AMBAS as dimensoes (antes so a altura) — saida
     * estreita + gap grande empurrava o stack pra fora e a largura caia a 1px. */
    if (n > 1) {
        int gmax = (g_wh - 2 * g_gap - n) / (n - 1);   /* vertical (n numa coluna) */
        if (gmax < 0) gmax = 0;
        if (g > gmax) g = gmax;
    }
    {
        int gmaxw = (g_ww - 2) / 3;   /* horizontal: outer(2g) + master|stack(g) */
        if (gmaxw < 0) gmaxw = 0;
        if (g > gmaxw) g = gmaxw;
    }
    if (n > 0) {
        int wx = g_wx + g, wy = g_wy + g;
        int ww = g_ww - 2 * g, wh = g_wh - 2 * g;
        if (ww < 1) ww = 1;
        if (wh < 1) wh = 1;

        int nm = n < g_nmaster ? n : g_nmaster;   /* masters exibidos */
        int ns = n - nm;                           /* stack           */
        int mw;
        if (ns == 0)      mw = ww;                 /* tudo master  */
        else if (nm == 0) mw = 0;                  /* tudo stack   */
        else              mw = (int)(ww * g_mfact) - g / 2;
        if (mw < 0) mw = 0;

        int i = 0;
        for (Client *c = g_clients; c; c = c->next) {
            if (c->ws != g_curws || c->floating)
                continue;
            int x, y, w, h;
            if (i < nm) {
                int base = (wh - (nm - 1) * g) / nm;
                y = wy + i * (base + g);
                h = (i == nm - 1) ? (wy + wh - y) : base;   /* #33 resto no ultimo */
                x = wx;
                w = (ns == 0) ? ww : mw;
            } else {
                int idx = i - nm;
                int base = (wh - (ns - 1) * g) / ns;
                y = wy + idx * (base + g);
                h = (idx == ns - 1) ? (wy + wh - y) : base;
                x = wx + (mw > 0 ? mw + g : 0);            /* #32 nmaster=0 sem gap */
                w = ww - mw - (mw > 0 ? g : 0);
            }
            if (h < 1) h = 1;
            if (w < 1) w = 1;
            wm_send("%s %u %d %d %d %d %d %d", CMD_PLACE, c->id, x, y, w, h,
                    g_curws, i);
            i++;
        }
    }

    /* floating: cascata, focado por cima (#30 sem sobrepor total, #31) */
    int fi = 0;
    for (Client *c = g_clients; c; c = c->next) {
        if (c->ws != g_curws || !c->floating)
            continue;
        int w = g_ww / 2, h = g_wh / 2;
        int step = fi * 28;
        int x = g_wx + (g_ww - w) / 2 + step;
        int y = g_wy + (g_wh - h) / 2 + step;
        if (x + w > g_wx + g_ww) x = g_wx + g_ww - w;
        if (y + h > g_wy + g_wh) y = g_wy + g_wh - h;
        int z = (c == g_focused) ? 2000 : (1000 + fi);
        wm_send("%s %u %d %d %d %d %d %d", CMD_PLACE, c->id, x, y, w, h,
                g_curws, z);
        fi++;
    }

    /* sempre manda FOCUS (0 = limpa) para nao deixar foco no ws antigo (#6) */
    wm_send("%s %u", CMD_FOCUS, g_focused ? g_focused->id : 0);
    wm_send(CMD_FRAME_COMMIT);
}

void focusstack(int dir)
{
    if (!g_focused)
        return;
    Client *sel = g_focused, *c = NULL;
    if (dir > 0) {
        for (c = sel->next; c && c->ws != g_curws; c = c->next);
        if (!c)
            for (c = g_clients; c && c->ws != g_curws; c = c->next);
    } else {
        Client *i;
        for (i = g_clients; i && i != sel; i = i->next)
            if (i->ws == g_curws)
                c = i;
        if (!c)
            for (i = g_clients; i; i = i->next)
                if (i->ws == g_curws)
                    c = i;
    }
    if (c) {
        g_focused = c;
        send_frame();   /* audit #23: re-declara com o novo foco -> o z do floating
                         * focado sobe atomicamente (FOCUS sozinho nao mexia no z) */
    }
}

void setmfact(float d)
{
    float f = g_mfact + d;
    if (f < 0.05f || f > 0.95f)
        return;
    g_mfact = f;
    send_frame();
}

void incnmaster(int d)
{
    g_nmaster += d;
    if (g_nmaster < 0)  g_nmaster = 0;
    if (g_nmaster > 16) g_nmaster = 16;   /* teto (#39: evita overflow/absurdo) */
    send_frame();
}

void view(int ws)
{
    if (ws == g_curws)
        return;
    g_curws = ws;                      /* WORKSPACE vai dentro do frame (send_frame) */
    g_focused = NULL;
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == ws) { g_focused = c; break; }
    send_frame();
}

void tagto(int ws)
{
    if (!g_focused || ws < 0 || ws >= NTUWM_WS)
        return;
    Client *moved = g_focused;
    moved->ws = ws;
    /* avisa o dispd do novo ws da janela movida DENTRO do frame (#28), sem
     * redimensionar (WS, nao PLACE 1x1 que destruiria a grade do terminal). */
    g_move_id = moved->id; g_move_ws = ws; g_move_pending = 1;
    /* foco vai pra outra janela do ws atual — #5 */
    g_focused = NULL;
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == g_curws) { g_focused = c; break; }
    send_frame();   /* re-tila o ws atual (a janela movida some) */
}

void zoom(void)
{
    Client *c = g_focused;
    if (!c || c == g_clients)
        return;
    c->floating = 0;   /* audit #25: zoom torna a janela master (tiled) — se ela
                        * fosse floating, ficava fora do tiling e o zoom nao surtia efeito */
    /* pop: destaca e re-anexa na cabeca (vira master) */
    Client **pp = &g_clients;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    *pp = c->next;
    c->next = g_clients;
    g_clients = c;
    send_frame();
}

void killfocused(void)
{
    if (g_focused)
        wm_send("%s %u", CMD_CLOSE, g_focused->id);
}

void togglefloating(void)
{
    if (g_focused) {
        g_focused->floating = !g_focused->floating;
        send_frame();   /* audit #26: o CMD_FLOAT agora vai DENTRO do frame (atomico
                         * com o layout), nao mais solto antes */
    }
}
