/*
 * input.c — teclado (hook LL global + fallback WM_KEYDOWN/UP) e mouse.
 *
 * O hook LL roda no MAIN thread (marshalado pelo message loop). Ele so decide
 * supressao e ENFILEIRA a tecla (rapido — hooks LL tem timeout ~300ms; escrever
 * no pty/ntwm poderia estourar e o Windows removeria o hook, #15). O
 * roteamento real (ntwm/terminal/app) roda em input_process_keys() no
 * frame_tick, fora do callback do hook.
 *
 * Pares down/up: a tecla suprimida no keydown tem o keyup suprimido tambem
 * (senao vaza um keyup sem keydown pro Win32, #11). Fila infalivel: nunca
 * suprime o que nao conseguiu enfileirar (#14). Fallback WM_KEYDOWN/UP fica
 * sempre disponivel (o hook suprime o que trata, entao nao ha duplicacao) —
 * assim, se o Windows remover o hook silenciosamente, a entrada continua (#16).
 */
#include "dispd.h"
#include "../common/ntuwm.h"

static HHOOK g_hook;
static unsigned char g_down[256];       /* teclas fisicamente pressionadas (#26) */
static unsigned char g_suppressed[256]; /* keydown suprimido -> suprime o keyup (#11) */
static unsigned char g_to_app[256];     /* keydown FOI entregue ao app -> keyup tambem (#10) */

/* fila de teclas (produtor: hook/WM_KEY*; consumidor: frame_tick — mesmo thread) */
typedef struct { unsigned mods; DWORD vk, scan; int down; } KeyEv;
#define KQCAP 256
static KeyEv g_kq[KQCAP];
static int g_kqh, g_kqt;

static int kq_push(unsigned mods, DWORD vk, DWORD scan, int down)
{
    int nt = (g_kqt + 1) % KQCAP;
    if (nt == g_kqh) return 0;   /* cheia */
    g_kq[g_kqt].mods = mods; g_kq[g_kqt].vk = vk; g_kq[g_kqt].scan = scan;
    g_kq[g_kqt].down = down;
    g_kqt = nt;
    return 1;
}

static int kq_pop(KeyEv *e)
{
    if (g_kqh == g_kqt) return 0;
    *e = g_kq[g_kqh];
    g_kqh = (g_kqh + 1) % KQCAP;
    return 1;
}

static int is_modifier(DWORD vk)
{
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
           vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
           vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
           vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU;
}

/* $mod = Alt ESQUERDO; Alt direito (AltGr) e' modificador de caractere (#22) */
static unsigned cur_mods(void)
{
    int altgr = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    unsigned m = 0;
    if (GetAsyncKeyState(VK_LMENU) & 0x8000)                     m |= MOD_ALT;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) && !altgr)       m |= MOD_CTRL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)                     m |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000))                    m |= MOD_WIN;
    return m;
}

/* teclas especiais -> sequencia VT (com modificadores para setas/nav, #28) */
static const char *special_seq(DWORD vk, unsigned mods)
{
    static char buf[16];
    const char *fin = NULL;
    switch (vk) {
    case VK_UP:    fin = "A"; break;
    case VK_DOWN:  fin = "B"; break;
    case VK_RIGHT: fin = "C"; break;
    case VK_LEFT:  fin = "D"; break;
    case VK_HOME:  fin = "H"; break;
    case VK_END:   fin = "F"; break;
    }
    if (fin) {
        int mc = 1 + ((mods & MOD_SHIFT) ? 1 : 0) + ((mods & MOD_ALT) ? 2 : 0) +
                     ((mods & MOD_CTRL) ? 4 : 0);
        if (mc > 1) snprintf(buf, sizeof buf, "\x1b[1;%d%s", mc, fin);
        else        snprintf(buf, sizeof buf, "\x1b[%s", fin);
        return buf;
    }
    /* nav/funcao com modificadores no formato CSI ~ (#26) */
    int code = 0;
    switch (vk) {
    case VK_PRIOR:  code = 5;  break;
    case VK_NEXT:   code = 6;  break;
    case VK_INSERT: code = 2;  break;
    case VK_DELETE: code = 3;  break;
    case VK_F5:  code = 15; break;   case VK_F6:  code = 17; break;
    case VK_F7:  code = 18; break;   case VK_F8:  code = 19; break;
    case VK_F9:  code = 20; break;   case VK_F10: code = 21; break;
    case VK_F11: code = 23; break;   case VK_F12: code = 24; break;
    }
    if (code) {
        int mc = 1 + ((mods & MOD_SHIFT) ? 1 : 0) + ((mods & MOD_ALT) ? 2 : 0) +
                     ((mods & MOD_CTRL) ? 4 : 0);
        if (mc > 1) snprintf(buf, sizeof buf, "\x1b[%d;%d~", code, mc);
        else        snprintf(buf, sizeof buf, "\x1b[%d~", code);
        return buf;
    }
    switch (vk) {
    case VK_RETURN: return "\r";
    case VK_BACK:   return "\x7f";
    case VK_ESCAPE: return "\x1b";
    case VK_TAB:    return (mods & MOD_SHIFT) ? "\x1b[Z" : "\t";   /* Shift+Tab #28 */
    case VK_F1:  return "\x1bOP";   case VK_F2:  return "\x1bOQ";
    case VK_F3:  return "\x1bOR";   case VK_F4:  return "\x1bOS";
    }
    return NULL;
}

/* binds embutidos quando nao ha ntwm (Alt+...) */
static int builtin_key(unsigned mods, DWORD vk)
{
    if (mods != MOD_ALT)
        return 0;
    if (vk == VK_RETURN) { spawn_terminal(NULL); return 1; }
    if (vk == VK_TAB) {
        /* audit #8: circula SO entre janelas visiveis do ws atual (nunca foca
         * uma janela oculta/de outro workspace) */
        Window *start = g_srv.focused, *n = NULL;
        for (Window *w = start ? start->next : g_srv.windows; w; w = w->next)
            if (w->visible && w->ws == g_srv.cur_ws) { n = w; break; }
        if (!n)                                  /* wrap: do inicio ate o foco */
            for (Window *w = g_srv.windows; w && w != start; w = w->next)
                if (w->visible && w->ws == g_srv.cur_ws) { n = w; break; }
        if (n) win_focus(n);
        return 1;
    }
    if (vk == 'Q') {
        if (g_srv.focused) dispd_close_window(g_srv.focused);   /* #54 */
        return 1;
    }
    return 0;
}

/* roteamento real (main thread, fora do hook) */
static void route_key(unsigned mods, DWORD vk, DWORD scan, int down)
{
    if (down && wmproto_connected() && wmproto_grabbed(mods, vk)) {
        wmproto_ev_key(mods, vk);
        return;
    }
    if (down && !wmproto_connected() && builtin_key(mods, vk))
        return;

    Window *f = g_srv.focused;
    if (!f)
        return;   /* nada focado: nao engole (#19) */

    /* traduz o caractere SO no keydown: ToUnicodeEx tem efeito colateral no
     * estado de dead keys/AltGr; chamar no keyup consumiria/produziria
     * composicao na fase errada (audit #14). No keyup r fica 0 (sem char). */
    WCHAR wb[8];
    int r = 0;
    if (down) {
        int altgr = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
        BYTE ks[256];
        memset(ks, 0, sizeof ks);
        if (mods & MOD_SHIFT) ks[VK_SHIFT] = 0x80;
        if (mods & MOD_CTRL)  ks[VK_CONTROL] = 0x80;
        if (altgr) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }
        if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
        r = ToUnicodeEx((UINT)vk, (UINT)scan, ks, wb, 8, 0, GetKeyboardLayout(0));
    }

    if (f->kind == WK_APP) {                    /* app: evento estruturado down/up */
        unsigned cp = 0;
        if (r >= 1) {                           /* codepoint completo (#24) */
            cp = (unsigned)wb[0];
            if (r >= 2 && wb[0] >= 0xD800 && wb[0] <= 0xDBFF &&
                wb[1] >= 0xDC00 && wb[1] <= 0xDFFF)
                cp = 0x10000u + (((unsigned)wb[0] - 0xD800) << 10) + ((unsigned)wb[1] - 0xDC00);
        }
        appsrv_input_key(f->id, mods, vk, cp, down);
        return;
    }
    if (!f->term || !down)                      /* terminal: so no keydown */
        return;

    /* abas do terminal (Ctrl+Shift+T/W, Ctrl+Tab) — nao vao pro pty */
    if ((mods & MOD_CTRL) && (mods & MOD_SHIFT) && vk == 'T') {
        win_tab_add(f, NULL);                   /* nova aba tty */
        return;
    }
    if ((mods & MOD_CTRL) && (mods & MOD_SHIFT) && vk == 'W') {
        win_tab_close(f, f->active_tab);        /* ultima aba -> fecha a janela */
        return;
    }
    if ((mods & MOD_CTRL) && vk == VK_TAB) {
        if (f->ntabs > 1) {
            int d = (mods & MOD_SHIFT) ? -1 : 1;
            win_tab_switch(f, (f->active_tab + d + f->ntabs) % f->ntabs);
        }
        return;
    }

    /* scrollback: Shift+PgUp/PgDn rola o historico (nao vai pro pty) */
    if ((mods & MOD_SHIFT) && (vk == VK_PRIOR || vk == VK_NEXT)) {
        int page = f->term->rows > 1 ? f->term->rows - 1 : 1;
        vt_scroll(f->term, vk == VK_PRIOR ? page : -page);
        return;
    }
    vt_scroll_reset(f->term);                    /* digitou -> volta pro fundo */

    const char *seq = special_seq(vk, mods);    /* teclas especiais -> VT */
    if (seq) { term_input(f->term, seq, (int)strlen(seq)); return; }

    if (r >= 1) {                               /* texto -> UTF-8 (#21) */
        char utf8[16];
        int un = WideCharToMultiByte(CP_UTF8, 0, wb, r, utf8, sizeof utf8, NULL, NULL);
        if (un > 0) {
            if (mods & MOD_ALT) {               /* Alt = Meta: ESC + char (#23) */
                char esc = 0x1b;
                term_input(f->term, &esc, 1);
            }
            term_input(f->term, utf8, un);
        }
    }
}

/* drena a fila de teclas (chamado no frame_tick, fora do hook) */
void input_process_keys(void)
{
    KeyEv e;
    while (kq_pop(&e))
        route_key(e.mods, e.vk, e.scan, e.down);
}

/* enfileira down/up; retorna 1 se a tecla deve ser suprimida (o hook usa isso) */
static int enqueue(DWORD vk, DWORD scan, int down)
{
    unsigned mods = cur_mods();
    int mod = is_modifier(vk);
    int idx = (int)(vk & 0xff);

    if (down) {
        int repeat = g_down[idx];
        int grab = !mod && wmproto_connected() && wmproto_grabbed(mods, vk);
        if (grab && repeat)
            return 1;                       /* #26: suprime auto-repeat em grabs */
        int app = g_srv.focused && g_srv.focused->kind == WK_APP;
        /* entrega: grab, ou janela focada (modifiers so vao pra app) */
        int want = grab || (g_srv.focused != NULL && (!mod || app));
        int suppress = grab || (g_srv.focused != NULL && !mod);
        int to_app = 0;
        if (want) {
            if (!kq_push(mods, vk, scan, 1)) {   /* #14: nao suprime o que nao enfileirou */
                /* audit #12: NAO marca g_down — senao o proximo auto-repeat
                 * viraria "repeat" e seria suprimido como se este tivesse sido
                 * entregue. Deixa g_down=0 pra a repeticao tentar de novo. */
                g_down[idx] = 0;
                dispd_log("input: fila cheia, tecla perdida");
                return 0;
            }
            g_srv.keys_seen++;
            g_srv.dirty = 1;
            to_app = (app && !grab);          /* #10: entregue AO APP (nao foi grab) */
        }
        g_down[idx] = 1;                      /* #12: so marca down apos entregar */
        g_suppressed[idx] = (unsigned char)(suppress ? 1 : 0);
        g_to_app[idx] = (unsigned char)(to_app ? 1 : 0);
        return suppress;
    } else {
        g_down[idx] = 0;
        int to_app = g_to_app[idx];
        g_to_app[idx] = 0;
        /* audit #10: so manda o keyup pro app se o KEYDOWN foi pro app (nao um
         * grab consumido pelo WM) — senao o app recebe um keyup orfao. */
        if (to_app) {
            if (!kq_push(mods, vk, scan, 0))     /* audit #11: checa o retorno */
                dispd_log("input: fila cheia, keyup perdido (mod pode ficar preso)");
        }
        int suppress = g_suppressed[idx];        /* suprime o par do keydown */
        g_suppressed[idx] = 0;
        return suppress;
    }
}

/* fallbacks WM_KEYDOWN/WM_KEYUP (sempre ativos; o hook suprime o que trata) */
void input_key(unsigned vk, unsigned scan)   { enqueue((DWORD)vk, (DWORD)scan, 1); }
void input_keyup(unsigned vk, unsigned scan) { enqueue((DWORD)vk, (DWORD)scan, 0); }

int input_hook_active(void)
{
    return g_hook != NULL;
}

/* ---- mouse (#8/#9): encaminha ao app ou terminal sob o cursor ---- */

void input_mouse(int sx, int sy, int button, int press, int motion)
{
    Window *w = win_at_point(sx, sy);
    if (!w)
        return;

    /* clique na barra de abas -> troca de aba */
    if (w->kind == WK_TERM && w->ntabs > 0 && button == 0 && press && !motion) {
        int tby = w->rect.top + w->border_px;
        if (g_srv.title_h > 0 && sy >= tby && sy < tby + g_srv.title_h) {
            int tix = w->rect.left + w->border_px;
            int tiw = (w->rect.right - w->rect.left) - 2 * w->border_px;
            int tw = tiw / w->ntabs;
            if (tw < 1) tw = 1;
            int ti = (sx - tix) / tw;
            if (ti >= 0 && ti < w->ntabs)
                win_tab_switch(w, ti);
            return;
        }
    }

    int ix = w->rect.left + w->border_px;
    int iy = w->rect.top + w->border_px + g_srv.title_h;
    int cx = sx - ix, cy = sy - iy;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (w->kind == WK_APP) {
        int buttons = ((GetKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0) |
                      ((GetKeyState(VK_RBUTTON) & 0x8000) ? 2 : 0) |
                      ((GetKeyState(VK_MBUTTON) & 0x8000) ? 4 : 0);
        if (motion && buttons == 0)
            return;   /* nao inunda o app com hover puro (relaciona #61) */
        appsrv_input_mouse(w->id, cx, cy, buttons);
    } else if (w->term) {
        int col = g_srv.cellw > 0 ? cx / g_srv.cellw : 0;
        int row = g_srv.cellh > 0 ? cy / g_srv.cellh : 0;
        if (button >= 64 && !w->term->on_alt) {   /* roda no shell -> scrollback */
            vt_scroll(w->term, button == 64 ? 3 : -3);
        } else {
            term_mouse(w->term, col, row, button, press, motion);
        }
    }
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (k->flags & LLKHF_INJECTED)             /* #29: ignora SendInput */
            return CallNextHookEx(g_hook, code, wp, lp);
        if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            if (enqueue(k->vkCode, k->scanCode, 0))
                return 1;                           /* suprime o keyup do par (#11) */
        } else if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            if (enqueue(k->vkCode, k->scanCode, 1))
                return 1;                           /* suprime a tecla tratada */
        }
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

void input_install_hook(void)
{
    char d[8] = "";
    GetEnvironmentVariableA("DISPD_DEBUG", d, sizeof d);
    g_srv.debug = 1;   /* TEMP DIAGNOSTICO: forca k:N/rx na barra (reverter p/ (d[0]=='1')) */
    (void)d;

    g_hook = SetWindowsHookExA(WH_KEYBOARD_LL, ll_proc, GetModuleHandleA(NULL), 0);
    if (!g_hook)
        dispd_log("input: SetWindowsHookEx(WH_KEYBOARD_LL) falhou (%lu) — usando WM_KEYDOWN",
                  GetLastError());
    else
        dispd_log("input: hook de teclado LL instalado");
}
