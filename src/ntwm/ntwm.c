/*
 * ntwm.c - loop de politica do desktop.
 *
 * Binds, ntwmctl e ntbar chegam aos mesmos dispatchers. O loop nunca bloqueia
 * indefinidamente no dispd: isso permite servir IPC enquanto nao ha evento de
 * janela e coalescer quadros aguardando FRAME-APPLIED.
 */
#include "ntwm.h"
#include <stdarg.h>

void wm_report(const char *fmt, ...)
{
    char msg[512], debug[560];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    snprintf(debug, sizeof debug, "ntwm: %s\n", msg);
    OutputDebugStringA(debug);
    if (g_pipe != INVALID_HANDLE_VALUE)
        wm_send("%s %s", CMD_NOTIFY, msg);
}

static void client_focus(unsigned id)
{
    Client *c = cl_find(id);
    if (!c)
        return;
    Workspace *ws = wm_workspace(c->ws);
    if (ws)
        ws->focused = c;
    wm_ipc_event("focus", c->title);
}

typedef struct PointerDrag {
    unsigned client_id;
    int active;
    int button;
    int sx, sy;
    int last_x, last_y;
    WmRect initial;
    int was_floating;
    int threshold_reached;
    unsigned edges;
} PointerDrag;

static PointerDrag g_drag;

static unsigned drag_edges(WmRect r, int x, int y)
{
    unsigned edges = x < r.x + r.w / 2 ? WM_EDGE_LEFT : WM_EDGE_RIGHT;
    edges |= y < r.y + r.h / 2 ? WM_EDGE_TOP : WM_EDGE_BOTTOM;
    return edges;
}

static int drag_threshold_reached(int x, int y)
{
    long long dx = (long long)x - g_drag.sx;
    long long dy = (long long)y - g_drag.sy;
    int threshold = g_wm.config.drag_threshold;
    return threshold <= 0 || dx * dx + dy * dy >=
        (long long)threshold * threshold;
}

static void resize_floating_drag(Client *c, int dx, int dy)
{
    int x = g_drag.initial.x;
    int y = g_drag.initial.y;
    int w = g_drag.initial.w;
    int h = g_drag.initial.h;
    if (g_drag.edges & WM_EDGE_RIGHT)
        w += dx;
    else {
        x += dx;
        w -= dx;
    }
    if (g_drag.edges & WM_EDGE_BOTTOM)
        h += dy;
    else {
        y += dy;
        h -= dy;
    }
    if (w < 80) {
        if (g_drag.edges & WM_EDGE_LEFT)
            x = g_drag.initial.x + g_drag.initial.w - 80;
        w = 80;
    }
    if (h < 60) {
        if (g_drag.edges & WM_EDGE_TOP)
            y = g_drag.initial.y + g_drag.initial.h - 60;
        h = 60;
    }
    wm_move_floating(c, x, y, w, h);
}

void wm_pointer_event(unsigned id, int x, int y, int button, int state,
                      unsigned mods)
{
    Client *c = cl_find(id);
    if (state == 1) {
        if (!c || !g_wm.config.mod ||
            (mods & g_wm.config.mod) != g_wm.config.mod)
            return;
        ZeroMemory(&g_drag, sizeof g_drag);
        g_drag.active = 1;
        g_drag.client_id = id;
        g_drag.button = button;
        g_drag.sx = x; g_drag.sy = y;
        g_drag.last_x = x; g_drag.last_y = y;
        g_drag.initial = c->rect;
        g_drag.was_floating = c->floating;
        g_drag.edges = drag_edges(c->rect, x, y);
        Workspace *ws = wm_workspace(c->ws);
        if (ws)
            ws->focused = c;
        return;
    }
    if (!g_drag.active || g_drag.client_id != id || g_drag.button != button)
        return;
    if (!c) {
        ZeroMemory(&g_drag, sizeof g_drag);
        return;
    }
    int dx = x - g_drag.sx, dy = y - g_drag.sy;
    if (state == 2) {
        if (!g_drag.threshold_reached) {
            if (!drag_threshold_reached(x, y))
                return;
            g_drag.threshold_reached = 1;
            if (c->fullscreen || c->maximized) {
                c->fullscreen = 0;
                c->maximized = 0;
            }
            if (button == 0 && !g_drag.was_floating)
                wm_client_set_floating(c, 1);
        }
        if (button == 0)
            wm_move_floating(c, g_drag.initial.x + dx, g_drag.initial.y + dy,
                             g_drag.initial.w, g_drag.initial.h);
        else if (button == 2 && g_drag.was_floating)
            resize_floating_drag(c, dx, dy);
        else if (button == 2)
            wm_resize_pointer(c, x - g_drag.last_x, y - g_drag.last_y,
                              g_drag.edges);
        g_drag.last_x = x;
        g_drag.last_y = y;
    } else {
        if (g_drag.threshold_reached && button == 0 && !g_drag.was_floating)
            wm_reinsert(c, x, y);
        ZeroMemory(&g_drag, sizeof g_drag);
    }
}

static void handle_pointer(char **av, int n)
{
    if (n < 7)
        return;
    wm_pointer_event((unsigned)strtoul(av[1], NULL, 10),
                     atoi(av[2]), atoi(av[3]), atoi(av[4]), atoi(av[5]),
                     (unsigned)strtoul(av[6], NULL, 16));
}

static void handle_line(char *line)
{
    char verb[32];
    char *sp = strchr(line, ' ');
    int vl = sp ? (int)(sp - line) : (int)strlen(line);
    if (vl >= (int)sizeof verb) vl = (int)sizeof verb - 1;
    memcpy(verb, line, (size_t)vl);
    verb[vl] = 0;

    int tail = -1;
    if (!strcmp(verb, EVT_WINDOW) || !strcmp(verb, EVT_WINDOW_V2))
        tail = 6;
    else if (!strcmp(verb, EVT_CREATED))
        tail = 4;
    else if (!strcmp(verb, EVT_TITLE))
        tail = 2;
    else if (!strcmp(verb, EVT_ERR))
        tail = 1;

    char *av[16];
    int n = ntuwm_split(line, av, 16, tail);
    if (!n)
        return;

    if (!strcmp(verb, EVT_WELCOME)) {
        int ver = n >= 3 ? atoi(av[2]) : 0;
        if (ver < NTUWM_PROTO_MIN || ver > NTUWM_PROTO_VER) {
            wm_report("protocolo dispd incompativel: %d", ver);
            g_wm.running = 0;
            return;
        }
        wm_register_grabs();
    } else if (!strcmp(verb, EVT_OUTPUT) && n >= 6) {
        g_wm.wx = atoi(av[2]);
        g_wm.wy = atoi(av[3]);
        g_wm.ww = atoi(av[4]);
        g_wm.wh = atoi(av[5]);
    } else if (!strcmp(verb, EVT_CURWS) && n >= 2) {
        int ws = atoi(av[1]);
        if (ws >= 0 && ws < NTUWM_WS)
            g_wm.cur_ws = ws;
    } else if (!strcmp(verb, EVT_WINDOW_V2) && n >= 7) {
        cl_add((unsigned)strtoul(av[1], NULL, 10), atoi(av[2]),
               (DWORD)strtoul(av[3], NULL, 10), atoi(av[4]),
               (unsigned)strtoul(av[5], NULL, 0), av[6]);
    } else if (!strcmp(verb, EVT_WINDOW) && n >= 5) {
        unsigned flags = n >= 6 && atoi(av[5]) ? NTUWM_STATE_FLOATING : 0;
        cl_add((unsigned)strtoul(av[1], NULL, 10), atoi(av[2]),
               (DWORD)strtoul(av[3], NULL, 10), atoi(av[4]), flags,
               n >= 7 ? av[6] : "");
    } else if (!strcmp(verb, EVT_CREATED) && n >= 4) {
        cl_add((unsigned)strtoul(av[1], NULL, 10), atoi(av[2]),
               (DWORD)strtoul(av[3], NULL, 10), g_wm.cur_ws, 0,
               n >= 5 ? av[4] : "");
        wm_request_frame();
    } else if (!strcmp(verb, EVT_DESTROYED) && n >= 2) {
        cl_remove((unsigned)strtoul(av[1], NULL, 10));
        wm_request_frame();
    } else if (!strcmp(verb, EVT_TITLE) && n >= 3) {
        cl_set_title(cl_find((unsigned)strtoul(av[1], NULL, 10)), av[2]);
    } else if (!strcmp(verb, EVT_FOCUSED) && n >= 2) {
        client_focus((unsigned)strtoul(av[1], NULL, 10));
    } else if (!strcmp(verb, EVT_KEY) && n >= 3) {
        wm_handle_key((unsigned)strtoul(av[1], NULL, 16),
                      (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(verb, EVT_FRAME_APPLIED) && n >= 2) {
        wm_frame_applied((unsigned)strtoul(av[1], NULL, 10));
    } else if (!strcmp(verb, EVT_SYNC) || !strcmp(verb, EVT_RESYNC)) {
        wm_send_snapshot();
        wm_ipc_snapshot();
    } else if (!strcmp(verb, EVT_PING)) {
        wm_send(CMD_PONG);
    } else if (!strcmp(verb, EVT_WSREQ) && n >= 2) {
        wm_view(atoi(av[1]));
    } else if (!strcmp(verb, EVT_MOVED) && n >= 6) {
        wm_move_floating(cl_find((unsigned)strtoul(av[1], NULL, 10)),
                         atoi(av[2]), atoi(av[3]), atoi(av[4]), atoi(av[5]));
    } else if (!strcmp(verb, EVT_POINTER)) {
        handle_pointer(av, n);
    } else if (!strcmp(verb, EVT_CONFIGURE)) {
        g_wm.waiting_serial = 0;
    } else if (!strcmp(verb, EVT_ERR)) {
        wm_report("dispd: %s", n >= 2 ? av[1] : "erro");
        wm_ipc_event("error", n >= 2 ? av[1] : "erro");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest"))
        return wm_selftest();
    if (argc > 1 && !strcmp(argv[1], "--check-config")) {
        WmConfig check;
        char error[320];
        return wm_config_load(&check, error, sizeof error) ? 0 : 2;
    }
    wm_config_defaults(&g_wm.config);
    wm_reload_config(1); /* erro mantem os defaults e sera visivel apos conectar */
    wm_state_init();
    wm_ipc_start();

    if (wm_connect() != 0) {
        wm_ipc_stop();
        return 1;
    }
    wm_send("%s ntwm %d", CMD_HELLO, NTUWM_PROTO_VER);

    char buf[8192];
    while (g_wm.running) {
        wm_ipc_drain();
        int n = wm_read(buf, sizeof buf);
        if (n < 0)
            break;
        if (n == 0) {
            Sleep(5);
            continue;
        }
        buf[n] = 0;
        for (char *line = strtok(buf, "\r\n"); line;
             line = strtok(NULL, "\r\n")) {
            ntu_trim(line);
            if (*line)
                handle_line(line);
        }
    }
    wm_ipc_stop();
    return 0;
}
