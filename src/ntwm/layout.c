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

Client *cl_add(unsigned id, int ws)
{
    if (cl_find(id))
        return NULL;
    Client *c = (Client *)calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->id = id;
    c->ws = ws;
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

/* dwm tile(): master-stack em duas colunas, com gaps uniformes. */
void send_frame(void)
{
    wm_send(CMD_FRAME_BEGIN);

    /* conta so os tiled (nao-floating) do ws atual */
    int n = 0;
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == g_curws && !c->floating)
            n++;

    int g = g_gap;
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
                h = (wh - (nm - 1) * g) / nm;
                x = wx;
                y = wy + i * (h + g);
                w = (ns == 0) ? ww : mw;
            } else {
                int idx = i - nm;
                h = (wh - (ns - 1) * g) / ns;
                x = wx + mw + g;
                y = wy + idx * (h + g);
                w = ww - mw - g;
            }
            if (h < 1) h = 1;
            wm_send("%s %u %d %d %d %d %d %d", CMD_PLACE, c->id, x, y, w, h,
                    g_curws, i);
            i++;
        }
    }

    /* floating: centralizado, por cima (z alto) */
    for (Client *c = g_clients; c; c = c->next) {
        if (c->ws != g_curws || !c->floating)
            continue;
        int w = g_ww / 2, h = g_wh / 2;
        int x = g_wx + (g_ww - w) / 2, y = g_wy + (g_wh - h) / 2;
        wm_send("%s %u %d %d %d %d %d %d", CMD_PLACE, c->id, x, y, w, h,
                g_curws, 1000);
    }

    if (g_focused)
        wm_send("%s %u", CMD_FOCUS, g_focused->id);
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
        wm_send("%s %u", CMD_FOCUS, c->id);
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
    if (g_nmaster < 0)
        g_nmaster = 0;
    send_frame();
}

void view(int ws)
{
    if (ws == g_curws)
        return;
    g_curws = ws;
    wm_send("%s %d", CMD_WORKSPACE, ws);
    g_focused = NULL;
    for (Client *c = g_clients; c; c = c->next)
        if (c->ws == ws) { g_focused = c; break; }
    send_frame();
}

void tagto(int ws)
{
    if (!g_focused)
        return;
    g_focused->ws = ws;
    send_frame();   /* re-tila o ws atual (a janela movida some) */
}

void zoom(void)
{
    Client *c = g_focused;
    if (!c || c == g_clients)
        return;
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
        send_frame();
    }
}
