/*
 * input.c — teclado via hook low-level global (WH_KEYBOARD_LL).
 *
 * Por que hook e nao WM_KEYDOWN: o dispd e' lancado pelo initd (processo nao
 * interativo), entao a janela raiz fica visivel/topmost mas NAO ganha foco de
 * teclado — os WM_KEYDOWN nunca chegam. O hook LL e' GLOBAL: recebe as teclas
 * independente de foco, que e' exatamente o que um compositor dono da tela
 * precisa (ver pesquisa nt-dwm-compositor §5). Evolucao futura: Raw Input.
 *
 * Regra: combo com grab do ntwm -> vira evento KEY (nao vai ao pty); senao ->
 * traduz e escreve no pty do terminal focado. Teclas tratadas sao suprimidas
 * (return 1) para nao vazarem a outras janelas.
 */
#include "dispd.h"
#include "../common/ntuwm.h"

static HHOOK g_hook;
static int   g_dbg;

static unsigned cur_mods(void)
{
    unsigned m = 0;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    m |= MOD_ALT;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) m |= MOD_CTRL;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   m |= MOD_SHIFT;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) || (GetAsyncKeyState(VK_RWIN) & 0x8000))
        m |= MOD_WIN;
    return m;
}

static void to_term(const char *bytes, int n)
{
    if (g_srv.focused && g_srv.focused->term)
        term_input(g_srv.focused->term, bytes, n);
}

/* teclas especiais -> sequencias VT (estilo xterm). Retorna 1 se tratou. */
static int special_key(DWORD vk)
{
    const char *seq = NULL;
    switch (vk) {
    case VK_UP:     seq = "\x1b[A"; break;
    case VK_DOWN:   seq = "\x1b[B"; break;
    case VK_RIGHT:  seq = "\x1b[C"; break;
    case VK_LEFT:   seq = "\x1b[D"; break;
    case VK_HOME:   seq = "\x1b[H"; break;
    case VK_END:    seq = "\x1b[F"; break;
    case VK_PRIOR:  seq = "\x1b[5~"; break;
    case VK_NEXT:   seq = "\x1b[6~"; break;
    case VK_INSERT: seq = "\x1b[2~"; break;
    case VK_DELETE: seq = "\x1b[3~"; break;
    case VK_RETURN: seq = "\r"; break;
    case VK_BACK:   seq = "\x7f"; break;
    case VK_TAB:    seq = "\t"; break;
    case VK_ESCAPE: seq = "\x1b"; break;
    case VK_F1:     seq = "\x1bOP"; break;
    case VK_F2:     seq = "\x1bOQ"; break;
    case VK_F3:     seq = "\x1bOR"; break;
    case VK_F4:     seq = "\x1bOS"; break;
    default: return 0;
    }
    to_term(seq, (int)strlen(seq));
    return 1;
}

/* binds embutidos quando nao ha ntwm (Alt+...). Retorna 1 se tratou. */
static int builtin_key(unsigned mods, DWORD vk)
{
    if (mods != MOD_ALT)
        return 0;
    if (vk == VK_RETURN) { spawn_terminal(NULL); return 1; }
    if (vk == VK_TAB) {
        Window *n = g_srv.focused && g_srv.focused->next ? g_srv.focused->next
                                                         : g_srv.windows;
        if (n) win_focus(n);
        return 1;
    }
    if (vk == 'Q') {
        if (g_srv.focused) win_destroy(g_srv.focused);
        return 1;
    }
    return 0;
}

/* retorna 1 se a tecla foi tratada (e deve ser suprimida) */
static int handle_key(DWORD vk, DWORD scan)
{
    /* modificadores puros passam direto */
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
        vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
        vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_LCONTROL ||
        vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU)
        return 0;

    unsigned mods = cur_mods();

    /* 1) grab do ntwm */
    if (wmproto_connected() && wmproto_grabbed(mods, (unsigned)vk)) {
        wmproto_ev_key(mods, (unsigned)vk);
        if (g_dbg) dispd_log("key vk=%lx mods=%x -> ntwm (grab)", vk, mods);
        return 1;
    }

    /* 2) binds embutidos (so sem ntwm) */
    if (!wmproto_connected() && builtin_key(mods, vk))
        return 1;

    /* 3) teclas especiais -> sequencia VT */
    if (special_key(vk)) {
        if (g_dbg) dispd_log("key vk=%lx -> term (especial)", vk);
        return 1;
    }

    /* 4) texto: traduz VK+scan -> char (respeita shift/caps/ctrl/layout).
     * NAO setamos Alt no estado: Alt e' o $mod, nao deve alterar o texto. */
    BYTE ks[256];
    memset(ks, 0, sizeof ks);
    if (mods & MOD_SHIFT) ks[VK_SHIFT] = 0x80;
    if (mods & MOD_CTRL)  ks[VK_CONTROL] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;

    WCHAR wb[8];
    int r = ToUnicodeEx((UINT)vk, (UINT)scan, ks, wb, 8, 0, GetKeyboardLayout(0));
    if (r >= 1 && wb[0]) {
        char c = (char)wb[0];
        to_term(&c, 1);
        if (g_dbg)
            dispd_log("key vk=%lx mods=%x -> term(%s) 0x%02x", vk, mods,
                      (g_srv.focused && g_srv.focused->term &&
                       g_srv.focused->term->be)
                          ? g_srv.focused->term->be->name : "nenhum",
                      (unsigned char)c);
        return 1;
    }
    if (g_dbg) dispd_log("key vk=%lx mods=%x -> nao tratada", vk, mods);
    return 0;
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        g_srv.keys_seen++;   /* prova (visivel na barra) que o hook disparou */
        g_srv.dirty = 1;     /* atualiza a barra na hora */
        if (handle_key(k->vkCode, k->scanCode))
            return 1;   /* suprime a tecla tratada */
    }
    return CallNextHookEx(g_hook, code, wp, lp);
}

void input_install_hook(void)
{
    char d[8] = "";
    GetEnvironmentVariableA("DISPD_DEBUG", d, sizeof d);
    g_dbg = (d[0] == '1');

    g_hook = SetWindowsHookExA(WH_KEYBOARD_LL, ll_proc, GetModuleHandleA(NULL), 0);
    if (!g_hook)
        dispd_log("input: SetWindowsHookEx(WH_KEYBOARD_LL) falhou (%lu)",
                  GetLastError());
    else
        dispd_log("input: hook de teclado LL instalado%s",
                  g_dbg ? " (debug on)" : "");
}
