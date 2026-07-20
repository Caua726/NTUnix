/* actions.c - dispatchers unificados para binds, IPC e ntbar. */
#include "ntwm.h"

static int parse_dir(const char *s, WmDirection *dir)
{
    if (!_stricmp(s, "left") || !_stricmp(s, "l")) { *dir = DIR_LEFT; return 1; }
    if (!_stricmp(s, "right") || !_stricmp(s, "r")) { *dir = DIR_RIGHT; return 1; }
    if (!_stricmp(s, "up") || !_stricmp(s, "u")) { *dir = DIR_UP; return 1; }
    if (!_stricmp(s, "down") || !_stricmp(s, "d")) { *dir = DIR_DOWN; return 1; }
    return 0;
}

static void reply_set(char *reply, size_t cap, const char *s)
{
    if (!reply || !cap)
        return;
    strncpy(reply, s, cap - 1);
    reply[cap - 1] = 0;
}

int wm_dispatch(const char *action, const char *arg, char *reply, size_t reply_cap)
{
    if (!action)
        return 0;
    if (!arg)
        arg = "";
    if (!_stricmp(action, "spawn")) {
        if (*arg) wm_send("%s %s", CMD_SPAWN, arg);
        else wm_send("%s", CMD_SPAWN);
    } else if (!_stricmp(action, "close")) {
        wm_close_focused();
    } else if (!_stricmp(action, "focus")) {
        WmDirection d;
        if (!parse_dir(arg, &d)) goto invalid;
        wm_focus_direction(d);
    } else if (!_stricmp(action, "move")) {
        WmDirection d;
        if (!parse_dir(arg, &d)) goto invalid;
        wm_move_direction(d);
    } else if (!_stricmp(action, "resize")) {
        char buf[64];
        strncpy(buf, arg, sizeof buf - 1);
        buf[sizeof buf - 1] = 0;
        char *space = strchr(buf, ' ');
        float amount = 0.05f;
        if (space) { *space++ = 0; amount = (float)atof(space); }
        if (amount < 0) amount = -amount;
        if (amount <= 0 || amount > 0.5f) goto invalid;
        WmDirection d;
        if (!parse_dir(buf, &d)) goto invalid;
        wm_resize_direction(d, amount);
    } else if (!_stricmp(action, "workspace")) {
        int ws = atoi(arg) - 1;
        if (ws < 0 || ws >= NTUWM_WS) goto invalid;
        wm_view(ws);
    } else if (!_stricmp(action, "movetoworkspace")) {
        int ws = atoi(arg) - 1;
        if (ws < 0 || ws >= NTUWM_WS) goto invalid;
        wm_move_to_workspace(ws);
    } else if (!_stricmp(action, "togglefloating")) {
        wm_toggle_floating();
    } else if (!_stricmp(action, "fullscreen")) {
        wm_toggle_fullscreen();
    } else if (!_stricmp(action, "maximize")) {
        wm_toggle_maximized();
    } else if (!_stricmp(action, "layout")) {
        Workspace *ws = wm_workspace(g_wm.cur_ws);
        if (!_stricmp(arg, "toggle"))
            wm_set_layout(ws && ws->layout == LAYOUT_DWINDLE
                ? LAYOUT_MASTER : LAYOUT_DWINDLE);
        else if (!_stricmp(arg, "dwindle"))
            wm_set_layout(LAYOUT_DWINDLE);
        else if (!_stricmp(arg, "master"))
            wm_set_layout(LAYOUT_MASTER);
        else goto invalid;
    } else if (!_stricmp(action, "layoutmsg")) {
        if (!wm_layout_message(arg)) goto invalid;
    } else if (!_stricmp(action, "reload")) {
        if (!wm_reload_config(0)) {
            reply_set(reply, reply_cap, "error reload recusado; configuracao anterior mantida");
            return 0;
        }
    } else if (!_stricmp(action, "quit")) {
        g_wm.running = 0;
    } else if (!_stricmp(action, "status")) {
        Workspace *ws = wm_workspace(g_wm.cur_ws);
        Client *f = ws ? ws->focused : NULL;
        int clients = 0, floating = 0;
        for (Client *c = ws ? ws->clients : NULL; c; c = c->ws_next) {
            clients++;
            floating += c->floating != 0;
        }
        snprintf(reply, reply_cap,
                 "ok workspace=%d layout=%s clients=%d floating=%d "
                 "focused=%u focused_floating=%d rect=%d,%d,%d,%d title=%s",
                 g_wm.cur_ws + 1,
                 ws && ws->layout == LAYOUT_MASTER ? "master" : "dwindle",
                 clients, floating, f ? f->id : 0, f ? f->floating : 0,
                 f ? f->rect.x : 0, f ? f->rect.y : 0,
                 f ? f->rect.w : 0, f ? f->rect.h : 0,
                 f ? f->title : "");
        return 1;
    } else if (!_stricmp(action, "snapshot")) {
        wm_ipc_snapshot();
    } else {
        goto invalid;
    }
    reply_set(reply, reply_cap, "ok");
    return 1;

invalid:
    reply_set(reply, reply_cap, "error dispatcher ou argumento invalido");
    return 0;
}

void wm_handle_key(unsigned mods, unsigned vk)
{
    for (int i = g_wm.config.nbinds - 1; i >= 0; i--) {
        WmBind *b = &g_wm.config.binds[i];
        if (b->mods == mods && b->vk == vk) {
            wm_dispatch(b->action, b->arg, NULL, 0);
            return;
        }
    }
}

void wm_register_grabs(void)
{
    wm_send(CMD_GRABS_BEGIN);
    /* Faz parte da mesma troca dos grabs: reload nunca deixa teclado novo com
     * modificador de mouse antigo (ou o inverso). */
    wm_send("%s %x", CMD_POINTER_MOD, g_wm.config.mod);
    for (int i = 0; i < g_wm.config.nbinds; i++)
        wm_send("%s %x %x", CMD_GRAB,
                g_wm.config.binds[i].mods, g_wm.config.binds[i].vk);
    wm_send(CMD_GRABS_COMMIT);
}
