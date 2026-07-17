/*
 * input.c — teclado via hook LL global (independe de foco) + fallback.
 *
 * O hook LL roda no MAIN thread (marshalado pelo message loop). Ele so decide
 * supressao e ENFILEIRA a tecla (rapido — hooks LL tem timeout ~300ms; escrever
 * no pty/ntwm poderia estourar e o Windows removeria o hook, #15). O
 * roteamento real (ntwm/terminal/app) roda em input_process_keys() no
 * frame_tick, fora do callback do hook.
 *
 * Roteia: grab do ntwm -> evento KEY; senao janela focada: terminal -> bytes
 * VT (UTF-8, teclas especiais com modificadores); app -> APP-KEY estruturado.
 */
#include "dispd.h"
#include "../common/ntuwm.h"

static HHOOK g_hook;
static unsigned char g_down[256];   /* teclas pressionadas (auto-repeat #26) */

/* fila de teclas (produtor: hook/WM_KEYDOWN; consumidor: frame_tick — mesmo
 * thread, sem lock) */
typedef struct { unsigned mods; DWORD vk, scan; } KeyEv;
#define KQCAP 128
static KeyEv g_kq[KQCAP];
static int g_kqh, g_kqt;

static void kq_push(unsigned mods, DWORD vk, DWORD scan)
{
    int nt = (g_kqt + 1) % KQCAP;
    if (nt == g_kqh) return;   /* cheia: descarta */
    g_kq[g_kqt].mods = mods; g_kq[g_kqt].vk = vk; g_kq[g_kqt].scan = scan;
    g_kqt = nt;
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
    switch (vk) {
    case VK_PRIOR:  return "\x1b[5~";
    case VK_NEXT:   return "\x1b[6~";
    case VK_INSERT: return "\x1b[2~";
    case VK_DELETE: return "\x1b[3~";
    case VK_RETURN: return "\r";
    case VK_BACK:   return "\x7f";
    case VK_ESCAPE: return "\x1b";
    case VK_TAB:    return (mods & MOD_SHIFT) ? "\x1b[Z" : "\t";   /* Shift+Tab #28 */
    case VK_F1:  return "\x1bOP";   case VK_F2:  return "\x1bOQ";
    case VK_F3:  return "\x1bOR";   case VK_F4:  return "\x1bOS";
    case VK_F5:  return "\x1b[15~"; case VK_F6:  return "\x1b[17~";
    case VK_F7:  return "\x1b[18~"; case VK_F8:  return "\x1b[19~";
    case VK_F9:  return "\x1b[20~"; case VK_F10: return "\x1b[21~";
    case VK_F11: return "\x1b[23~"; case VK_F12: return "\x1b[24~";   /* F5-F12 #27 */
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

/* roteamento real (main thread, fora do hook) */
static void route_key(unsigned mods, DWORD vk, DWORD scan)
{
    if (wmproto_connected() && wmproto_grabbed(mods, vk)) {
        wmproto_ev_key(mods, vk);
        return;
    }
    if (!wmproto_connected() && builtin_key(mods, vk))
        return;

    Window *f = g_srv.focused;
    if (!f)
        return;   /* nada focado: nao engole (#19) */

    /* traduz o caractere (respeita shift/caps/ctrl/AltGr; layout do usuario) */
    int altgr = (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0;
    BYTE ks[256];
    memset(ks, 0, sizeof ks);
    if (mods & MOD_SHIFT) ks[VK_SHIFT] = 0x80;
    if (mods & MOD_CTRL)  ks[VK_CONTROL] = 0x80;
    if (altgr) { ks[VK_CONTROL] = 0x80; ks[VK_MENU] = 0x80; }
    if (GetKeyState(VK_CAPITAL) & 1) ks[VK_CAPITAL] = 1;
    WCHAR wb[8];
    int r = ToUnicodeEx((UINT)vk, (UINT)scan, ks, wb, 8, 0, GetKeyboardLayout(0));

    if (f->kind == WK_APP) {                    /* app: evento estruturado (#2) */
        appsrv_input_key(f->id, mods, vk, (r >= 1) ? (unsigned)wb[0] : 0);
        return;
    }
    if (!f->term)
        return;

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
        route_key(e.mods, e.vk, e.scan);
}

/* enfileira uma tecla; retorna 1 se deve ser suprimida (o hook usa isso) */
static int enqueue_key(DWORD vk, DWORD scan)
{
    if (is_modifier(vk))
        return 0;
    unsigned mods = cur_mods();
    int grab = wmproto_connected() && wmproto_grabbed(mods, vk);
    int repeat = g_down[vk & 0xff];
    g_down[vk & 0xff] = 1;
    if (grab && repeat)
        return 1;                       /* #26: suprime auto-repeat em acoes do WM */
    int suppress = grab || (g_srv.focused != NULL);
    if (suppress) {
        kq_push(mods, vk, scan);
        g_srv.keys_seen++;
        g_srv.dirty = 1;
    }
    return suppress;
}

/* fallback WM_KEYDOWN (so quando o hook LL nao esta ativo) */
void input_key(unsigned vk, unsigned scan)
{
    enqueue_key((DWORD)vk, (DWORD)scan);
}

int input_hook_active(void)
{
    return g_hook != NULL;
}

static LRESULT CALLBACK ll_proc(int code, WPARAM wp, LPARAM lp)
{
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (k->flags & LLKHF_INJECTED)             /* #29: ignora SendInput */
            return CallNextHookEx(g_hook, code, wp, lp);
        if (wp == WM_KEYUP || wp == WM_SYSKEYUP) {
            g_down[k->vkCode & 0xff] = 0;           /* limpa estado (auto-repeat) */
            return CallNextHookEx(g_hook, code, wp, lp);
        }
        if (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN) {
            if (enqueue_key(k->vkCode, k->scanCode))
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
