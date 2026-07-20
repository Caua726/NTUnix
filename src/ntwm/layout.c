/*
 * layout.c - estado por workspace e algoritmos master/dwindle.
 *
 * A arvore dwindle segue o mecanismo essencial do Hyprland: cada cliente e uma
 * folha; uma insercao cria um pai no lugar da folha focada; uma remocao promove
 * o irmao e elimina o pai. A geometria produzida aqui e sempre o destino
 * logico. O dispd mantem a geometria visual animada separadamente.
 */
#include "ntwm.h"
#include <math.h>

WmState g_wm;

static const LayoutOps *layout_ops_for(LayoutKind kind);

static int ws_valid(int ws)
{
    return ws >= 0 && ws < NTUWM_WS;
}

Workspace *wm_workspace(int index)
{
    return ws_valid(index) ? &g_wm.workspaces[index] : NULL;
}

static WmRect rect_make(int x, int y, int w, int h)
{
    WmRect r = { x, y, w < 1 ? 1 : w, h < 1 ? 1 : h };
    return r;
}

static DwindleNode *node_new(Client *c)
{
    DwindleNode *n = (DwindleNode *)calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->client = c;
    n->ratio = 0.5f;
    if (c)
        c->dwindle_leaf = n;
    return n;
}

static void workspace_list_add(Workspace *ws, Client *c)
{
    c->ws_next = NULL;
    if (!ws->clients || ws->insert_master) {
        c->ws_next = ws->clients;
        ws->clients = c;
    } else {
        Client *tail = ws->clients;
        while (tail->ws_next)
            tail = tail->ws_next;
        tail->ws_next = c;
    }
}

static void workspace_list_remove(Workspace *ws, Client *c)
{
    Client **pp = &ws->clients;
    while (*pp && *pp != c)
        pp = &(*pp)->ws_next;
    if (*pp)
        *pp = c->ws_next;
    c->ws_next = NULL;
    if (ws->focused == c)
        ws->focused = NULL;
}

static Client *first_tiled(Workspace *ws)
{
    for (Client *c = ws->clients; c; c = c->ws_next)
        if (!c->floating)
            return c;
    return NULL;
}

enum {
    DROP_AUTO = -1,
    DROP_LEFT = 0,
    DROP_RIGHT,
    DROP_TOP,
    DROP_BOTTOM
};

static void dwindle_insert_side(Workspace *ws, Client *c, Client *on, int side)
{
    DwindleNode *leaf = node_new(c);
    if (!leaf)
        exit(1); /* initd respawna e o snapshot reconstroi sem divergencia */
    if (!ws->dwindle_root) {
        ws->dwindle_root = leaf;
        return;
    }

    DwindleNode *old = on && on->dwindle_leaf ? on->dwindle_leaf : ws->dwindle_root;
    while (old && !old->client)
        old = old->child[0] ? old->child[0] : old->child[1];
    if (!old) {
        free(leaf);
        c->dwindle_leaf = NULL;
        return;
    }

    DwindleNode *parent = node_new(NULL);
    if (!parent) {
        free(leaf);
        c->dwindle_leaf = NULL;
        exit(1);
    }
    parent->parent = old->parent;
    parent->ratio = g_wm.config.dwindle_default_ratio / 100.0f;
    if (parent->ratio < 0.10f) parent->ratio = 0.10f;
    if (parent->ratio > 0.90f) parent->ratio = 0.90f;
    if (side == DROP_LEFT || side == DROP_RIGHT) {
        parent->vertical = 1;
        parent->child[0] = side == DROP_LEFT ? leaf : old;
        parent->child[1] = side == DROP_LEFT ? old : leaf;
    } else if (side == DROP_TOP || side == DROP_BOTTOM) {
        parent->vertical = 0;
        parent->child[0] = side == DROP_TOP ? leaf : old;
        parent->child[1] = side == DROP_TOP ? old : leaf;
    } else {
        parent->vertical = old->client && old->client->rect.w > 0
            ? old->client->rect.w >= old->client->rect.h
            : g_wm.ww >= g_wm.wh;
        parent->child[0] = old;
        parent->child[1] = leaf;
    }
    old->parent = parent;
    leaf->parent = parent;

    if (!parent->parent)
        ws->dwindle_root = parent;
    else if (parent->parent->child[0] == old)
        parent->parent->child[0] = parent;
    else
        parent->parent->child[1] = parent;
}

static void dwindle_insert_on(Workspace *ws, Client *c, Client *on)
{
    dwindle_insert_side(ws, c, on, DROP_AUTO);
}

static void dwindle_remove(Workspace *ws, Client *c)
{
    DwindleNode *leaf = c->dwindle_leaf;
    if (!leaf)
        return;
    DwindleNode *parent = leaf->parent;
    if (!parent) {
        ws->dwindle_root = NULL;
        free(leaf);
        c->dwindle_leaf = NULL;
        return;
    }
    DwindleNode *sibling = parent->child[0] == leaf ? parent->child[1] : parent->child[0];
    sibling->parent = parent->parent;
    if (!parent->parent)
        ws->dwindle_root = sibling;
    else if (parent->parent->child[0] == parent)
        parent->parent->child[0] = sibling;
    else
        parent->parent->child[1] = sibling;
    free(leaf);
    free(parent);
    c->dwindle_leaf = NULL;
}

static void workspace_attach(Workspace *ws, Client *c, Client *split_on)
{
    c->ws = ws->index;
    workspace_list_add(ws, c);
    if (!c->floating)
        ws->ops->insert(ws, c, split_on);
    ws->focused = c;
}

static void workspace_detach(Workspace *ws, Client *c)
{
    if (!c->floating)
        ws->ops->remove(ws, c);
    workspace_list_remove(ws, c);
    if (!ws->focused)
        ws->focused = ws->clients;
}

void wm_state_init(void)
{
    WmConfig cfg = g_wm.config;
    ZeroMemory(&g_wm, sizeof g_wm);
    g_wm.config = cfg;
    g_wm.cur_ws = 0;
    g_wm.next_serial = 1;
    g_wm.running = 1;
    for (int i = 0; i < NTUWM_WS; i++) {
        Workspace *ws = &g_wm.workspaces[i];
        ws->index = i;
        ws->layout = cfg.workspace[i].set_layout
            ? cfg.workspace[i].layout : cfg.default_layout;
        ws->ops = layout_ops_for(ws->layout);
        ws->nmaster = cfg.workspace[i].set_nmaster
            ? cfg.workspace[i].nmaster : cfg.nmaster;
        ws->mfact = cfg.workspace[i].set_mfact
            ? cfg.workspace[i].mfact : cfg.mfact;
        ws->orientation = cfg.workspace[i].set_orientation
            ? cfg.workspace[i].orientation : cfg.master_orientation;
        ws->insert_master = cfg.insert_master;
        ws->gap_inner = cfg.gap_inner;
        ws->gap_outer = cfg.gap_outer;
        if (cfg.workspace[i].name[0])
            strncpy(ws->name, cfg.workspace[i].name, sizeof ws->name - 1);
        else
            snprintf(ws->name, sizeof ws->name, "%d", i + 1);
    }
}

Client *cl_find(unsigned id)
{
    for (Client *c = g_wm.clients; c; c = c->next)
        if (c->id == id)
            return c;
    return NULL;
}

static void client_exe(Client *c)
{
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, c->pid);
    if (!p)
        return;
    DWORD cap = (DWORD)sizeof c->exe;
    if (!QueryFullProcessImageNameA(p, 0, c->exe, &cap))
        c->exe[0] = 0;
    CloseHandle(p);
}

Client *cl_add(unsigned id, int kind, DWORD pid, int ws, unsigned flags,
               const char *title)
{
    Client *old = cl_find(id);
    if (old)
        return old;
    Client *c = (Client *)calloc(1, sizeof *c);
    if (!c)
        exit(1);
    c->id = id;
    c->kind = kind;
    c->pid = pid;
    c->ws = ws_valid(ws) ? ws : g_wm.cur_ws;
    c->floating = (flags & NTUWM_STATE_FLOATING) != 0;
    c->fullscreen = (flags & NTUWM_STATE_FULLSCREEN) != 0;
    c->maximized = (flags & NTUWM_STATE_MAXIMIZED) != 0;
    c->style = g_wm.config.style;
    c->tile_weight = 1.0f;
    strncpy(c->title, title && *title ? title : (kind == 0 ? "terminal" : "app"),
            sizeof c->title - 1);
    client_exe(c);
    wm_apply_rules(c);
    if (!ws_valid(c->ws))
        c->ws = g_wm.cur_ws;
    c->next = g_wm.clients;
    g_wm.clients = c;
    Workspace *target = wm_workspace(c->ws);
    workspace_attach(target, c, target->focused);
    c->managed = 1;
    wm_ipc_event("client-added", c->title);
    return c;
}

void cl_remove(unsigned id)
{
    Client **pp = &g_wm.clients;
    while (*pp && (*pp)->id != id)
        pp = &(*pp)->next;
    if (!*pp)
        return;
    Client *c = *pp;
    *pp = c->next;
    Workspace *ws = wm_workspace(c->ws);
    if (ws)
        workspace_detach(ws, c);
    char data[32];
    snprintf(data, sizeof data, "%u", id);
    wm_ipc_event("client-removed", data);
    free(c);
}

void cl_set_title(Client *c, const char *title)
{
    if (!c || !title)
        return;
    strncpy(c->title, title, sizeof c->title - 1);
    c->title[sizeof c->title - 1] = 0;
    wm_apply_rules(c);
    wm_ipc_event("title", c->title);
}

void wm_client_set_workspace(Client *c, int wsidx)
{
    if (!c || !ws_valid(wsidx) || c->ws == wsidx)
        return;
    if (!c->managed) {
        c->ws = wsidx;
        return;
    }
    Workspace *old = wm_workspace(c->ws);
    Workspace *next = wm_workspace(wsidx);
    workspace_detach(old, c);
    workspace_attach(next, c, next->focused);
}

void wm_client_set_floating(Client *c, int floating)
{
    if (!c)
        return;
    floating = floating != 0;
    if (c->floating == floating)
        return;
    if (!c->managed) {
        c->floating = floating;
        return;
    }
    Workspace *ws = wm_workspace(c->ws);
    if (!ws)
        return;
    if (floating) {
        c->float_rect = c->rect;
        c->has_float_rect = 1;
        ws->ops->remove(ws, c);
        c->floating = 1;
    } else {
        c->floating = 0;
        ws->ops->insert(ws, c, first_tiled(ws));
    }
}

static WmRect workarea(Workspace *ws)
{
    int g = ws->gap_outer;
    int maxg = g_wm.ww < g_wm.wh ? g_wm.ww / 3 : g_wm.wh / 3;
    if (g > maxg) g = maxg;
    return rect_make(g_wm.wx + g, g_wm.wy + g,
                     g_wm.ww - 2 * g, g_wm.wh - 2 * g);
}

static WmRect inset_half_gap(WmRect r, int gap, int left, int top, int right, int bottom)
{
    int a = gap / 2;
    int b = gap - a;
    r.x += left ? b : 0;
    r.y += top ? b : 0;
    r.w -= (left ? b : 0) + (right ? a : 0);
    r.h -= (top ? b : 0) + (bottom ? a : 0);
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    return r;
}

static void arrange_dwindle_node(DwindleNode *n, WmRect r, int gap,
                                  int left, int top, int right, int bottom)
{
    if (!n)
        return;
    n->rect = r;
    if (n->client) {
        n->client->rect = inset_half_gap(r, gap, left, top, right, bottom);
        return;
    }
    float ratio = n->ratio;
    if (ratio < 0.10f) ratio = 0.10f;
    if (ratio > 0.90f) ratio = 0.90f;
    if (n->vertical) {
        int first = (int)(r.w * ratio);
        if (first < 1) first = 1;
        if (first >= r.w) first = r.w - 1;
        WmRect a = rect_make(r.x, r.y, first, r.h);
        WmRect b = rect_make(r.x + first, r.y, r.w - first, r.h);
        arrange_dwindle_node(n->child[0], a, gap, left, top, 1, bottom);
        arrange_dwindle_node(n->child[1], b, gap, 1, top, right, bottom);
    } else {
        int first = (int)(r.h * ratio);
        if (first < 1) first = 1;
        if (first >= r.h) first = r.h - 1;
        WmRect a = rect_make(r.x, r.y, r.w, first);
        WmRect b = rect_make(r.x, r.y + first, r.w, r.h - first);
        arrange_dwindle_node(n->child[0], a, gap, left, top, right, 1);
        arrange_dwindle_node(n->child[1], b, gap, left, 1, right, bottom);
    }
}

static float client_weight(Client *c)
{
    return c && c->tile_weight >= 0.10f ? c->tile_weight : 1.0f;
}

static void split_column(Client **items, int n, WmRect r, int gap)
{
    if (n <= 0)
        return;
    int total = r.h - gap * (n - 1);
    if (total < n) total = n;
    float weight_left = 0.0f;
    for (int i = 0; i < n; i++)
        weight_left += client_weight(items[i]);
    int pixels_left = total;
    int y = r.y;
    for (int i = 0; i < n; i++) {
        float weight = client_weight(items[i]);
        int h = i == n - 1 ? pixels_left :
            (int)(pixels_left * weight / weight_left + 0.5f);
        int reserve = n - i - 1;
        if (h < 1) h = 1;
        if (h > pixels_left - reserve) h = pixels_left - reserve;
        items[i]->rect = rect_make(r.x, y, r.w, h);
        y += h + gap;
        pixels_left -= h;
        weight_left -= weight;
    }
}

static void split_row(Client **items, int n, WmRect r, int gap)
{
    if (n <= 0)
        return;
    int total = r.w - gap * (n - 1);
    if (total < n) total = n;
    float weight_left = 0.0f;
    for (int i = 0; i < n; i++)
        weight_left += client_weight(items[i]);
    int pixels_left = total;
    int x = r.x;
    for (int i = 0; i < n; i++) {
        float weight = client_weight(items[i]);
        int w = i == n - 1 ? pixels_left :
            (int)(pixels_left * weight / weight_left + 0.5f);
        int reserve = n - i - 1;
        if (w < 1) w = 1;
        if (w > pixels_left - reserve) w = pixels_left - reserve;
        items[i]->rect = rect_make(x, r.y, w, r.h);
        x += w + gap;
        pixels_left -= w;
        weight_left -= weight;
    }
}

static void arrange_master(Workspace *ws, WmRect area)
{
    Client *items[NTU_MAX_WINDOWS];
    int n = 0;
    for (Client *c = ws->clients; c && n < NTU_MAX_WINDOWS; c = c->ws_next)
        if (!c->floating)
            items[n++] = c;
    if (!n)
        return;
    int nm = ws->nmaster;
    if (nm < 0) nm = 0;
    if (nm > n) nm = n;
    int ns = n - nm;
    int g = ws->gap_inner;
    if (!ns || !nm) {
        if (ws->orientation == MASTER_TOP || ws->orientation == MASTER_BOTTOM)
            split_row(items, n, area, g);
        else
            split_column(items, n, area, g);
        return;
    }

    if (ws->orientation == MASTER_CENTER) {
        int mw = (int)(area.w * ws->mfact);
        if (mw < 1) mw = 1;
        if (mw > area.w - 2) mw = area.w - 2;
        int side = (area.w - mw - 2 * g) / 2;
        if (side < 1) side = 1;
        WmRect master = rect_make(area.x + side + g, area.y, mw, area.h);
        split_column(items, nm, master, g);
        Client *left[NTU_MAX_WINDOWS], *right[NTU_MAX_WINDOWS];
        int nl = 0, nr = 0;
        for (int i = nm; i < n; i++)
            if (((i - nm) & 1) == 0) left[nl++] = items[i];
            else right[nr++] = items[i];
        split_column(left, nl, rect_make(area.x, area.y, side, area.h), g);
        split_column(right, nr,
                     rect_make(master.x + master.w + g, area.y,
                               area.x + area.w - (master.x + master.w + g), area.h), g);
        return;
    }

    if (ws->orientation == MASTER_LEFT || ws->orientation == MASTER_RIGHT) {
        int mw = (int)((area.w - g) * ws->mfact);
        if (mw < 1) mw = 1;
        if (mw >= area.w - g) mw = area.w - g - 1;
        WmRect m = rect_make(area.x, area.y, mw, area.h);
        WmRect s = rect_make(area.x + mw + g, area.y, area.w - mw - g, area.h);
        if (ws->orientation == MASTER_RIGHT) {
            WmRect t = m; m = s; s = t;
            m.x = area.x + area.w - mw;
            m.w = mw;
            s.x = area.x;
            s.w = area.w - mw - g;
        }
        split_column(items, nm, m, g);
        split_column(items + nm, ns, s, g);
    } else {
        int mh = (int)((area.h - g) * ws->mfact);
        if (mh < 1) mh = 1;
        if (mh >= area.h - g) mh = area.h - g - 1;
        WmRect m = rect_make(area.x, area.y, area.w, mh);
        WmRect s = rect_make(area.x, area.y + mh + g, area.w, area.h - mh - g);
        if (ws->orientation == MASTER_BOTTOM) {
            m.y = area.y + area.h - mh;
            s.y = area.y;
            s.h = area.h - mh - g;
        }
        split_row(items, nm, m, g);
        split_row(items + nm, ns, s, g);
    }
}

static void arrange_workspace(Workspace *ws)
{
    WmRect area = workarea(ws);
    ws->ops->arrange(ws, area);

    int fi = 0;
    for (Client *c = ws->clients; c; c = c->ws_next) {
        if (!c->floating)
            continue;
        if (c->has_float_rect)
            c->rect = c->float_rect;
        else {
            int w = g_wm.ww / 2, h = g_wm.wh / 2;
            int step = (fi++ % 10) * 28;
            c->rect = rect_make(g_wm.wx + (g_wm.ww - w) / 2 + step,
                                g_wm.wy + (g_wm.wh - h) / 2 + step, w, h);
        }
    }
    for (Client *c = ws->clients; c; c = c->ws_next)
        if (c->fullscreen || c->maximized)
            c->rect = rect_make(g_wm.wx, g_wm.wy, g_wm.ww, g_wm.wh);
}

static void send_frame_now(void)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (!ws || g_wm.ww < 1 || g_wm.wh < 1)
        return;
    arrange_workspace(ws);
    unsigned serial = g_wm.next_serial++;
    if (serial == 0)
        serial = g_wm.next_serial++;
    g_wm.waiting_serial = serial;

    wm_send("%s %u", CMD_FRAME_BEGIN, serial);
    wm_send("%s %d %d %d %d %d", CMD_ANIMATIONS, g_wm.config.animations,
            g_wm.config.move_ms, g_wm.config.open_ms,
            g_wm.config.workspace_ms, g_wm.config.focus_ms);
    wm_send("%s %d", CMD_WORKSPACE, g_wm.cur_ws);

    int z = 0;
    for (Client *c = g_wm.clients; c; c = c->next) {
        wm_send("%s %u %d", CMD_SETWS, c->id, c->ws);
        wm_send("%s %u %d %d %d", CMD_STATE, c->id, c->floating,
                c->fullscreen, c->maximized);
        wm_send("%s %u %d %06x %d %d %d %d %d", CMD_STYLE, c->id,
                c->style.border, c->style.border_rgb, c->style.opacity,
                c->style.shadow, c->style.radius, c->style.animate,
                c->style.titlebar);
        if (c->ws != g_wm.cur_ws)
            continue;
        int cz = c->floating ? 1000 + z : z;
        if (c->maximized) cz = 3000;
        if (c->fullscreen) cz = 4000;
        wm_send("%s %u %d %d %d %d %d %d", CMD_PLACE, c->id,
                c->rect.x, c->rect.y, c->rect.w, c->rect.h, c->ws, cz);
        z++;
    }
    wm_send("%s %u", CMD_FOCUS, ws->focused ? ws->focused->id : 0);
    wm_send("%s %u", CMD_FRAME_COMMIT, serial);
}

void wm_request_frame(void)
{
    if (g_wm.waiting_serial) {
        g_wm.frame_pending = 1;
        return;
    }
    send_frame_now();
}

void wm_frame_applied(unsigned serial)
{
    if (!g_wm.waiting_serial || serial != g_wm.waiting_serial)
        return;
    g_wm.waiting_serial = 0;
    wm_ipc_event("frame", "applied");
    if (g_wm.frame_pending) {
        g_wm.frame_pending = 0;
        send_frame_now();
    }
}

void wm_send_snapshot(void)
{
    g_wm.waiting_serial = 0;
    g_wm.frame_pending = 0;
    wm_request_frame();
}

static int center_x(const Client *c) { return c->rect.x + c->rect.w / 2; }
static int center_y(const Client *c) { return c->rect.y + c->rect.h / 2; }

static Client *geometric_candidate(Workspace *ws, Client *from, WmDirection dir)
{
    Client *best = NULL;
    long best_score = 0x7fffffffL;
    int fx = center_x(from), fy = center_y(from);
    for (Client *c = ws->clients; c; c = c->ws_next) {
        if (c == from)
            continue;
        int dx = center_x(c) - fx, dy = center_y(c) - fy;
        int primary, secondary;
        if (dir == DIR_LEFT) { if (dx >= 0) continue; primary = -dx; secondary = abs(dy); }
        else if (dir == DIR_RIGHT) { if (dx <= 0) continue; primary = dx; secondary = abs(dy); }
        else if (dir == DIR_UP) { if (dy >= 0) continue; primary = -dy; secondary = abs(dx); }
        else { if (dy <= 0) continue; primary = dy; secondary = abs(dx); }
        long score = (long)primary * 1024L + secondary;
        if (score < best_score) { best = c; best_score = score; }
    }
    return best;
}

void wm_focus_direction(WmDirection dir)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (!ws || !ws->focused)
        return;
    arrange_workspace(ws);
    Client *c = ws->ops->focus(ws, ws->focused, dir);
    if (c) {
        ws->focused = c;
        wm_ipc_event("focus", c->title);
        wm_request_frame();
    }
}

static void workspace_swap_order(Workspace *ws, Client *a, Client *b)
{
    Client *items[NTU_MAX_WINDOWS];
    int n = 0, ia = -1, ib = -1;
    for (Client *c = ws->clients; c && n < NTU_MAX_WINDOWS; c = c->ws_next) {
        if (c == a) ia = n;
        if (c == b) ib = n;
        items[n++] = c;
    }
    if (ia < 0 || ib < 0)
        return;
    Client *t = items[ia]; items[ia] = items[ib]; items[ib] = t;
    ws->clients = n ? items[0] : NULL;
    for (int i = 0; i < n; i++)
        items[i]->ws_next = i + 1 < n ? items[i + 1] : NULL;
}

void wm_move_direction(WmDirection dir)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (!ws || !ws->focused)
        return;
    arrange_workspace(ws);
    Client *other = ws->ops->focus(ws, ws->focused, dir);
    if (!other)
        return;
    ws->ops->move(ws, ws->focused, other);
    wm_request_frame();
}

void wm_resize_direction(WmDirection dir, float amount)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    Client *c = ws ? ws->focused : NULL;
    if (!c)
        return;
    if (c->floating) {
        if (!c->has_float_rect)
            c->float_rect = c->rect;
        int delta = (int)(amount * 400.0f);
        if (dir == DIR_LEFT) {
            int right = c->float_rect.x + c->float_rect.w;
            c->float_rect.w -= delta;
            if (c->float_rect.w < 80) c->float_rect.w = 80;
            c->float_rect.x = right - c->float_rect.w;
        } else if (dir == DIR_RIGHT) {
            c->float_rect.w += delta;
        } else if (dir == DIR_UP) {
            int bottom = c->float_rect.y + c->float_rect.h;
            c->float_rect.h -= delta;
            if (c->float_rect.h < 60) c->float_rect.h = 60;
            c->float_rect.y = bottom - c->float_rect.h;
        } else {
            c->float_rect.h += delta;
        }
        c->has_float_rect = 1;
    } else {
        ws->ops->resize(ws, c, dir, amount);
    }
    wm_request_frame();
}

void wm_view(int wsidx)
{
    if (!ws_valid(wsidx) || wsidx == g_wm.cur_ws)
        return;
    g_wm.cur_ws = wsidx;
    Workspace *ws = wm_workspace(wsidx);
    if (!ws->focused)
        ws->focused = ws->clients;
    char data[32];
    snprintf(data, sizeof data, "%d", wsidx + 1);
    wm_ipc_event("workspace", data);
    wm_request_frame();
}

void wm_move_to_workspace(int wsidx)
{
    Workspace *src = wm_workspace(g_wm.cur_ws);
    Client *c = src ? src->focused : NULL;
    Workspace *dst = wm_workspace(wsidx);
    if (!c || !dst || src == dst)
        return;
    workspace_detach(src, c);
    workspace_attach(dst, c, dst->focused);
    src->focused = src->clients;
    wm_request_frame();
}

void wm_toggle_floating(void)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    Client *c = ws ? ws->focused : NULL;
    if (!c)
        return;
    wm_client_set_floating(c, !c->floating);
    wm_request_frame();
}

void wm_toggle_fullscreen(void)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (ws && ws->focused) {
        ws->focused->fullscreen = !ws->focused->fullscreen;
        wm_request_frame();
    }
}

void wm_toggle_maximized(void)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (ws && ws->focused) {
        ws->focused->maximized = !ws->focused->maximized;
        wm_request_frame();
    }
}

void wm_set_layout(LayoutKind layout)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (!ws || (layout != LAYOUT_DWINDLE && layout != LAYOUT_MASTER))
        return;
    wm_workspace_set_layout(ws, layout);
    wm_ipc_event("layout", layout == LAYOUT_DWINDLE ? "dwindle" : "master");
    wm_request_frame();
}

void wm_workspace_set_layout(Workspace *ws, LayoutKind layout)
{
    if (!ws || (layout != LAYOUT_DWINDLE && layout != LAYOUT_MASTER))
        return;
    ws->layout = layout;
    ws->ops = layout_ops_for(layout);
}

int wm_layout_message(const char *message)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (!ws || !message)
        return 0;
    if (!ws->ops->message(ws, message))
        return 0;
    wm_request_frame();
    return 1;
}

void wm_close_focused(void)
{
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    if (ws && ws->focused)
        wm_send("%s %u", CMD_CLOSE, ws->focused->id);
}

void wm_move_floating(Client *c, int x, int y, int w, int h)
{
    if (!c)
        return;
    if (w < 80) w = 80;
    if (h < 60) h = 60;
    if (g_wm.ww > 0 && w > g_wm.ww * 4) w = g_wm.ww * 4;
    if (g_wm.wh > 0 && h > g_wm.wh * 4) h = g_wm.wh * 4;
    if (!c->floating)
        wm_client_set_floating(c, 1);
    c->float_rect = rect_make(x, y, w, h);
    c->has_float_rect = 1;
    wm_request_frame();
}

static int point_in_rect(WmRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static Client *drop_target(Workspace *ws, Client *c, int x, int y)
{
    Client *nearest = NULL;
    long long best = 0x7fffffffffffffffLL;
    for (Client *o = ws->clients; o; o = o->ws_next) {
        if (o == c || o->floating)
            continue;
        if (point_in_rect(o->rect, x, y))
            return o;
        long long dx = (long long)center_x(o) - x;
        long long dy = (long long)center_y(o) - y;
        long long d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            nearest = o;
        }
    }
    return nearest;
}

static int drop_side(Client *target, int x, int y)
{
    if (!target)
        return DROP_AUTO;
    long long dx = (long long)x - center_x(target);
    long long dy = (long long)y - center_y(target);
    long long ax = dx < 0 ? -dx : dx;
    long long ay = dy < 0 ? -dy : dy;
    if (ax * target->rect.h >= ay * target->rect.w)
        return dx < 0 ? DROP_LEFT : DROP_RIGHT;
    return dy < 0 ? DROP_TOP : DROP_BOTTOM;
}

static void workspace_reorder_drop(Workspace *ws, Client *c, Client *target,
                                   int x, int y)
{
    if (!ws || !c || !target || c == target)
        return;
    Client **unlink = &ws->clients;
    while (*unlink && *unlink != c)
        unlink = &(*unlink)->ws_next;
    if (*unlink != c)
        return;
    *unlink = c->ws_next;
    c->ws_next = NULL;

    int vertical_stack = ws->orientation == MASTER_LEFT ||
                         ws->orientation == MASTER_RIGHT ||
                         ws->orientation == MASTER_CENTER;
    int before = vertical_stack ? y < center_y(target) : x < center_x(target);
    Client **slot = &ws->clients;
    while (*slot && *slot != target)
        slot = &(*slot)->ws_next;
    if (!*slot) {
        while (*slot)
            slot = &(*slot)->ws_next;
        *slot = c;
    } else if (before) {
        c->ws_next = *slot;
        *slot = c;
    } else {
        c->ws_next = target->ws_next;
        target->ws_next = c;
    }
}

void wm_reinsert(Client *c, int x, int y)
{
    if (!c)
        return;
    Workspace *ws = wm_workspace(c->ws);
    if (!ws)
        return;
    if (c->dwindle_leaf)
        ws->ops->remove(ws, c);
    Client *nearest = drop_target(ws, c, x, y);
    int side = drop_side(nearest, x, y);
    if (ws->layout == LAYOUT_MASTER && nearest)
        workspace_reorder_drop(ws, c, nearest, x, y);
    c->floating = 0;
    dwindle_insert_side(ws, c, nearest, side);
    ws->focused = c;
    wm_request_frame();
}

/* ---- interface de layout ---- */

static void ops_insert(Workspace *ws, Client *c, Client *focused)
{
    /* A arvore e mantida tambem quando master esta ativo, permitindo alternar
     * layout sem perder o arranjo dwindle daquele workspace. */
    dwindle_insert_on(ws, c, focused);
}

static void ops_remove(Workspace *ws, Client *c)
{
    dwindle_remove(ws, c);
}

static void ops_arrange_dwindle(Workspace *ws, WmRect area)
{
    arrange_dwindle_node(ws->dwindle_root, area, ws->gap_inner, 0, 0, 0, 0);
}

static void ops_arrange_master(Workspace *ws, WmRect area)
{
    arrange_master(ws, area);
}

static Client *ops_focus(Workspace *ws, Client *from, WmDirection dir)
{
    return geometric_candidate(ws, from, dir);
}

static void ops_move_dwindle(Workspace *ws, Client *from, Client *to)
{
    (void)ws;
    if (!from->dwindle_leaf || !to->dwindle_leaf)
        return;
    DwindleNode *a = from->dwindle_leaf, *b = to->dwindle_leaf;
    a->client = to; to->dwindle_leaf = a;
    b->client = from; from->dwindle_leaf = b;
}

static void ops_move_master(Workspace *ws, Client *from, Client *to)
{
    workspace_swap_order(ws, from, to);
}

static int dwindle_resize_edge(Client *c, int vertical, int first_boundary,
                                int delta)
{
    DwindleNode *n = c ? c->dwindle_leaf : NULL;
    while (n && n->parent) {
        DwindleNode *p = n->parent;
        if (p->vertical == vertical &&
            ((first_boundary && p->child[0] == n) ||
             (!first_boundary && p->child[1] == n))) {
            int span = vertical ? p->rect.w : p->rect.h;
            if (span < 1)
                return 0;
            p->ratio += (float)delta / span;
            if (p->ratio < 0.10f) p->ratio = 0.10f;
            if (p->ratio > 0.90f) p->ratio = 0.90f;
            return 1;
        }
        n = p;
    }
    return 0;
}

static int dwindle_resize_pointer(Client *c, int dx, int dy, unsigned edges)
{
    int changed = 0;
    if (dx && (edges & WM_EDGE_RIGHT)) {
        int axis = dwindle_resize_edge(c, 1, 1, dx);
        changed |= axis ? axis : dwindle_resize_edge(c, 1, 0, dx);
    } else if (dx && (edges & WM_EDGE_LEFT)) {
        int axis = dwindle_resize_edge(c, 1, 0, dx);
        changed |= axis ? axis : dwindle_resize_edge(c, 1, 1, dx);
    }
    if (dy && (edges & WM_EDGE_BOTTOM)) {
        int axis = dwindle_resize_edge(c, 0, 1, dy);
        changed |= axis ? axis : dwindle_resize_edge(c, 0, 0, dy);
    } else if (dy && (edges & WM_EDGE_TOP)) {
        int axis = dwindle_resize_edge(c, 0, 0, dy);
        changed |= axis ? axis : dwindle_resize_edge(c, 0, 1, dy);
    }
    return changed;
}

static int rect_overlap_axis(WmRect a, WmRect b, int vertical)
{
    int lo = vertical ? (a.x > b.x ? a.x : b.x)
                      : (a.y > b.y ? a.y : b.y);
    int ahi = vertical ? a.x + a.w : a.y + a.h;
    int bhi = vertical ? b.x + b.w : b.y + b.h;
    int hi = ahi < bhi ? ahi : bhi;
    return hi - lo;
}

static Client *master_axis_neighbor(Workspace *ws, Client *c, int vertical,
                                    int positive)
{
    Client *best = NULL;
    int best_distance = 0x7fffffff;
    int origin = vertical ? center_y(c) : center_x(c);
    for (Client *o = ws->clients; o; o = o->ws_next) {
        if (o == c || o->floating || rect_overlap_axis(c->rect, o->rect, vertical) <= 0)
            continue;
        int pos = vertical ? center_y(o) : center_x(o);
        int distance = positive ? pos - origin : origin - pos;
        if (distance > 0 && distance < best_distance) {
            best_distance = distance;
            best = o;
        }
    }
    return best;
}

static int master_resize_pair(Workspace *ws, Client *c, int vertical,
                              int positive_edge, int delta)
{
    Client *neighbor = master_axis_neighbor(ws, c, vertical, positive_edge);
    if (!neighbor)
        return 0;
    float cw = client_weight(c);
    float nw = client_weight(neighbor);
    int span = vertical ? c->rect.h + neighbor->rect.h
                        : c->rect.w + neighbor->rect.w;
    if (span < 1)
        return 0;
    /* Na borda inferior/direita, delta positivo aumenta c. Na superior/
     * esquerda, delta positivo avanca a origem e portanto reduz c. */
    float change = (float)delta * (cw + nw) / span;
    if (!positive_edge)
        change = -change;
    if (cw + change < 0.10f)
        change = 0.10f - cw;
    if (nw - change < 0.10f)
        change = nw - 0.10f;
    if (change > -0.0001f && change < 0.0001f)
        return 0;
    c->tile_weight = cw + change;
    neighbor->tile_weight = nw - change;
    return 1;
}

static int master_resize_pointer(Workspace *ws, Client *c, int dx, int dy,
                                 unsigned edges)
{
    if (!ws || !c)
        return 0;
    WmRect area = workarea(ws);
    int changed = 0;
    int tiled = 0;
    for (Client *o = ws->clients; o; o = o->ws_next)
        tiled += !o->floating;
    int nm = ws->nmaster;
    if (nm < 0) nm = 0;
    if (nm > tiled) nm = tiled;
    int has_split = nm > 0 && tiled > nm;

    if (has_split && (ws->orientation == MASTER_LEFT ||
                      ws->orientation == MASTER_RIGHT) && dx) {
        float delta = (float)dx / (area.w > 0 ? area.w : 1);
        if (ws->orientation == MASTER_RIGHT)
            delta = -delta;
        ws->mfact += delta;
        changed = 1;
    } else if (has_split && (ws->orientation == MASTER_TOP ||
                             ws->orientation == MASTER_BOTTOM) && dy) {
        float delta = (float)dy / (area.h > 0 ? area.h : 1);
        if (ws->orientation == MASTER_BOTTOM)
            delta = -delta;
        ws->mfact += delta;
        changed = 1;
    } else if (has_split && ws->orientation == MASTER_CENTER && dx) {
        int middle = area.x + area.w / 2;
        float delta = 2.0f * dx / (area.w > 0 ? area.w : 1);
        if (center_x(c) < middle ||
            (center_x(c) == middle && (edges & WM_EDGE_LEFT)))
            delta = -delta;
        ws->mfact += delta;
        changed = 1;
    }
    if (ws->mfact < 0.10f) ws->mfact = 0.10f;
    if (ws->mfact > 0.90f) ws->mfact = 0.90f;

    int vertical_stack = ws->orientation == MASTER_LEFT ||
                         ws->orientation == MASTER_RIGHT ||
                         ws->orientation == MASTER_CENTER;
    if (vertical_stack && dy) {
        if (edges & WM_EDGE_BOTTOM)
            changed |= master_resize_pair(ws, c, 1, 1, dy);
        else if (edges & WM_EDGE_TOP)
            changed |= master_resize_pair(ws, c, 1, 0, dy);
    } else if (!vertical_stack && dx) {
        if (edges & WM_EDGE_RIGHT)
            changed |= master_resize_pair(ws, c, 0, 1, dx);
        else if (edges & WM_EDGE_LEFT)
            changed |= master_resize_pair(ws, c, 0, 0, dx);
    }
    return changed;
}

static void ops_resize_dwindle(Workspace *ws, Client *c,
                               WmDirection dir, float amount)
{
    arrange_workspace(ws);
    int px = (int)(amount * (dir == DIR_LEFT || dir == DIR_RIGHT
        ? g_wm.ww : g_wm.wh));
    if (px < 1) px = 1;
    if (dir == DIR_LEFT)
        dwindle_resize_pointer(c, -px, 0, WM_EDGE_LEFT);
    else if (dir == DIR_RIGHT)
        dwindle_resize_pointer(c, px, 0, WM_EDGE_RIGHT);
    else if (dir == DIR_UP)
        dwindle_resize_pointer(c, 0, -px, WM_EDGE_TOP);
    else
        dwindle_resize_pointer(c, 0, px, WM_EDGE_BOTTOM);
}

static void ops_resize_master(Workspace *ws, Client *c,
                              WmDirection dir, float amount)
{
    arrange_workspace(ws);
    int dx = 0, dy = 0;
    unsigned edge = WM_EDGE_NONE;
    if (dir == DIR_LEFT) {
        dx = -(int)(amount * g_wm.ww); edge = WM_EDGE_LEFT;
    } else if (dir == DIR_RIGHT) {
        dx = (int)(amount * g_wm.ww); edge = WM_EDGE_RIGHT;
    } else if (dir == DIR_UP) {
        dy = -(int)(amount * g_wm.wh); edge = WM_EDGE_TOP;
    } else {
        dy = (int)(amount * g_wm.wh); edge = WM_EDGE_BOTTOM;
    }
    master_resize_pointer(ws, c, dx, dy, edge);
}

void wm_resize_pointer(Client *c, int dx, int dy, unsigned edges)
{
    Workspace *ws = c ? wm_workspace(c->ws) : NULL;
    if (!ws || c->floating || (!dx && !dy))
        return;
    arrange_workspace(ws);
    if (ws->layout == LAYOUT_MASTER)
        master_resize_pointer(ws, c, dx, dy, edges);
    else
        dwindle_resize_pointer(c, dx, dy, edges);
    wm_request_frame();
}

static int ops_message_dwindle(Workspace *ws, const char *message)
{
    Client *c = ws->focused;
    if (!_stricmp(message, "togglesplit")) {
        if (!c || !c->dwindle_leaf || !c->dwindle_leaf->parent)
            return 0;
        c->dwindle_leaf->parent->vertical =
            !c->dwindle_leaf->parent->vertical;
        return 1;
    }
    if (!_strnicmp(message, "splitratio ", 11)) {
        if (!c || !c->dwindle_leaf || !c->dwindle_leaf->parent)
            return 0;
        c->dwindle_leaf->parent->ratio += (float)atof(message + 11);
        if (c->dwindle_leaf->parent->ratio < 0.10f)
            c->dwindle_leaf->parent->ratio = 0.10f;
        if (c->dwindle_leaf->parent->ratio > 0.90f)
            c->dwindle_leaf->parent->ratio = 0.90f;
        return 1;
    }
    return 0;
}

static int ops_message_master(Workspace *ws, const char *message)
{
    if (!_strnicmp(message, "orientation ", 12)) {
        const char *o = message + 12;
        if (!_stricmp(o, "left")) ws->orientation = MASTER_LEFT;
        else if (!_stricmp(o, "right")) ws->orientation = MASTER_RIGHT;
        else if (!_stricmp(o, "top")) ws->orientation = MASTER_TOP;
        else if (!_stricmp(o, "bottom")) ws->orientation = MASTER_BOTTOM;
        else if (!_stricmp(o, "center")) ws->orientation = MASTER_CENTER;
        else return 0;
        return 1;
    }
    if (!_strnicmp(message, "nmaster ", 8)) {
        ws->nmaster = atoi(message + 8);
        if (ws->nmaster < 0) ws->nmaster = 0;
        if (ws->nmaster > 32) ws->nmaster = 32;
        return 1;
    }
    return 0;
}

static const LayoutOps g_dwindle_ops = {
    "dwindle", ops_insert, ops_remove, ops_arrange_dwindle, ops_focus,
    ops_move_dwindle, ops_resize_dwindle, ops_message_dwindle
};

static const LayoutOps g_master_ops = {
    "master", ops_insert, ops_remove, ops_arrange_master, ops_focus,
    ops_move_master, ops_resize_master, ops_message_master
};

static const LayoutOps *layout_ops_for(LayoutKind kind)
{
    return kind == LAYOUT_MASTER ? &g_master_ops : &g_dwindle_ops;
}
