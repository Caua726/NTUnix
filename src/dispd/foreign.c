/*
 * foreign.c — gerência de janelas nativas do Windows (modelo komorebi/GlazeWM).
 *
 * O dispd é um app rodando sobre o win32k (não substitui ele). Toda janela — a
 * nossa e a do taskmgr/notepad/browser — é um HWND do win32k. Aqui a gente vira
 * o WINDOW MANAGER dessas janelas estrangeiras: descobre via WinEventHook +
 * EnumWindows, tira a moldura nativa, e posiciona/tila via SetWindowPos, tratando
 * cada uma como uma WK_FOREIGN no nosso modelo (o ntwm tila junto com os
 * terminais). Não compomos o conteúdo delas — o win32k desenha; a gente só manda
 * na geometria, foco e borda.
 */
#include "dispd.h"

static void foreign_resnap(HWND h);   /* fwd: usado em win_event, definido abaixo */

static Window *win_by_hwnd(HWND h)
{
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->kind == WK_FOREIGN && w->hwnd == h)
            return w;
    return NULL;
}

/* uma janela gerenciável = janela de app real de outro processo (não a nossa,
 * não a shell, não tool/diálogo sem título) */
static int manageable(HWND h)
{
    if (!h || !IsWindowVisible(h) || IsIconic(h))
        return 0;
    if (GetWindow(h, GW_OWNER))            /* dona (diálogo/tooltip) -> pula */
        return 0;
    LONG style = GetWindowLongA(h, GWL_STYLE);
    LONG ex = GetWindowLongA(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW)
        return 0;
    if (!(style & WS_CAPTION))              /* sem barra de título -> não é app */
        return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() || h == g_srv.root)
        return 0;                           /* a gente mesmo */
    if (GetWindowTextLengthA(h) == 0)
        return 0;
    char cls[64] = "";
    GetClassNameA(h, cls, sizeof cls);
    if (!strcmp(cls, "Shell_TrayWnd") || !strcmp(cls, "Progman") ||
        !strcmp(cls, "WorkerW") || !strcmp(cls, "Button") ||
        !strcmp(cls, "ntudispd"))
        return 0;                           /* shell / classe nossa */
    return 1;
}

static void foreign_add(HWND h)
{
    if (win_by_hwnd(h) || !manageable(h))
        return;
    Window *w = win_create_foreign(h);
    if (!w)
        return;
    w->orig_style = GetWindowLongPtrA(h, GWL_STYLE);
    GetWindowTextA(h, w->title, sizeof w->title - 1);
    GetWindowThreadProcessId(h, &w->pid);
    /* tira a moldura nativa: a nossa chrome (borda + barra) desenha em volta */
    LONG_PTR s = w->orig_style &
                 ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_BORDER | WS_DLGFRAME);
    SetWindowLongPtrA(h, GWL_STYLE, s);
    SetWindowPos(h, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
    win_focus(w);
    wmproto_ev_created(w);   /* ntwm tila junto com os terminais */
    dispd_log("foreign: gerenciando '%s' (hwnd %p)", w->title, (void *)h);
}

static void foreign_del(HWND h)
{
    Window *w = win_by_hwnd(h);
    if (!w)
        return;
    w->hwnd = NULL;          /* já sumiu: não mexer mais no HWND */
    dispd_close_window(w);
}

static void CALLBACK win_event(HWINEVENTHOOK hook, DWORD event, HWND h,
                               LONG idObj, LONG idChild, DWORD tid, DWORD tm)
{
    (void)hook; (void)tid; (void)tm;
    if (idObj != OBJID_WINDOW || idChild != CHILDID_SELF || !h)
        return;
    switch (event) {
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_CREATE:
        foreign_add(h);
        break;
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_HIDE:
        foreign_del(h);
        break;
    case EVENT_OBJECT_NAMECHANGE: {
        Window *w = win_by_hwnd(h);
        if (w) {
            GetWindowTextA(h, w->title, sizeof w->title - 1);
            wmproto_ev_title(w);
            g_srv.dirty = 1;
        }
        break;
    }
    case EVENT_SYSTEM_FOREGROUND: {
        Window *w = win_by_hwnd(h);   /* Windows mudou o foco -> sincroniza */
        if (w && g_srv.focused != w) {
            win_focus(w);
            wmproto_ev_focused(w);
        }
        break;
    }
    case EVENT_OBJECT_LOCATIONCHANGE:  /* usuario arrastou -> volta pro tile */
        foreign_resnap(h);
        break;
    default:
        break;
    }
}

static BOOL CALLBACK enum_win(HWND h, LPARAM p)
{
    (void)p;
    foreign_add(h);
    return TRUE;
}

void foreign_init(void)
{
    EnumWindows(enum_win, 0);   /* janelas já abertas no boot */
    UINT f = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, win_event, 0, 0, f);
    SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE, NULL, win_event, 0, 0, f); /* 0x8000..0x8003 */
    SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL, win_event, 0, 0, f);
    SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE,
                    NULL, win_event, 0, 0, f);   /* re-snap ao arrastar */
}

void foreign_place(Window *w, int x, int y, int cw, int ch)
{
    if (!w || !w->hwnd || !IsWindow(w->hwnd))
        return;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    SetRect(&w->fg_target, x, y, x + cw, y + ch);   /* alvo p/ o re-snap */
    SetWindowPos(w->hwnd, NULL, x, y, cw, ch, SWP_NOZORDER | SWP_NOACTIVATE);
}

/* O taskmgr (e apps de frame custom DWM) desenha a propria barra no client-area,
 * entao continua arrastavel mesmo sem WS_CAPTION. Quando a janela gerenciada sai
 * do tile, snap de volta pro alvo — assim ela nao pode ser movida pra fora. */
static void foreign_resnap(HWND h)
{
    Window *w = win_by_hwnd(h);
    if (!w || !w->hwnd || !IsWindow(w->hwnd))
        return;
    if (w->fg_target.right <= w->fg_target.left)
        return;                             /* ainda nao foi colocada */
    RECT r;
    if (!GetWindowRect(w->hwnd, &r))
        return;
    int dx = (int)(r.left - w->fg_target.left), dy = (int)(r.top - w->fg_target.top);
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx < 10 && dy < 10)
        return;                             /* perto o bastante -> nao briga */
    SetWindowPos(w->hwnd, NULL, w->fg_target.left, w->fg_target.top,
                 w->fg_target.right - w->fg_target.left,
                 w->fg_target.bottom - w->fg_target.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void foreign_focus(Window *w)
{
    if (!w || !w->hwnd || !IsWindow(w->hwnd))
        return;
    SetForegroundWindow(w->hwnd);
}

void foreign_release(Window *w)
{
    if (!w || !w->hwnd || !IsWindow(w->hwnd))
        return;
    SetWindowLongPtrA(w->hwnd, GWL_STYLE, w->orig_style);   /* devolve a moldura */
    SetWindowPos(w->hwnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED | SWP_NOACTIVATE);
}
