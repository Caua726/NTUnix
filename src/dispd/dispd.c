/*
 * dispd.c — display server / compositor do NTUnix (main).
 *
 * Cria a janela raiz fullscreen (WS_POPUP topmost — como o cmd do WinPE
 * pinta), sobe o servidor de protocolo (ntwm), hospeda terminais e roda o
 * loop de composicao. Sem ntwm conectado, um layout embutido mantem os
 * terminais usaveis (como um X server sem WM).
 *
 * C puro, -mwindows, ANSI. Convencoes do runtime NTUnix (ntu.h).
 * Compilado a 0x0601; ConPTY/DXGI ficam em TUs isoladas a 0x0A00.
 */
#include "dispd.h"
#include <stdarg.h>

Server g_srv;

void dispd_log(const char *fmt, ...)
{
    char line[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);

    char ts[32], p[MAX_PATH];
    ntu_now(ts, sizeof ts);
    ntu_path("/var/log/dispd.log", p, sizeof p);
    FILE *f = fopen(p, "ab");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, line);
        fclose(f);
    }
}

/* cmdline do shell padrao (tty): busybox ash, ou cmd.exe se DISPD_SHELL=cmd */
static const char *resolve_shell(const char *cmdline, char *buf, size_t cap)
{
    if (cmdline)
        return cmdline;
    char shell[32] = "";
    GetEnvironmentVariableA("DISPD_SHELL", shell, sizeof shell);
    if (!_stricmp(shell, "cmd")) {
        snprintf(buf, cap, "cmd.exe");
    } else {
        char bb[MAX_PATH];
        ntu_path("/system/bin/busybox.exe", bb, sizeof bb);
        snprintf(buf, cap, "\"%s\" ash -i", bb);
    }
    return buf;
}

Window *spawn_terminal(const char *cmdline)
{
    int cols = 80, rows = 24;
    Window *w = win_create(WK_TERM, cols * g_srv.cellw, rows * g_srv.cellh);
    if (!w)
        return NULL;
    strncpy(w->title, "terminal", sizeof w->title - 1);
    if (win_tab_add(w, cmdline) != 0) {   /* 1a aba */
        dispd_log("spawn_terminal: term_create falhou");
        win_destroy(w);
        return NULL;
    }
    w->pid = w->term->pid;   /* #41 */
    win_focus(w);
    wmproto_ev_created(w);
    dispd_log("terminal criado (id %u, backend %s)", w->id,
              w->term->be ? w->term->be->name : "?");
    return w;
}

/* ---- abas (tabs) do terminal: uma janela hospeda varias sessoes ---- */

int win_tab_add(Window *w, const char *cmdline)
{
    if (!w || w->kind != WK_TERM || w->ntabs >= MAX_TABS)
        return -1;
    char def[MAX_PATH + 64];
    cmdline = resolve_shell(cmdline, def, sizeof def);   /* NULL -> tty (ash) */
    int cols = g_srv.cellw > 0 ? w->cw / g_srv.cellw : 80;
    int rows = g_srv.cellh > 0 ? w->ch / g_srv.cellh : 24;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    Terminal *t = term_create(cmdline, cols, rows, w);
    if (!t)
        return -1;
    w->tabs[w->ntabs++] = t;
    w->active_tab = w->ntabs - 1;
    w->term = t;                 /* aba ativa */
    t->dirty = 1;
    w->dirty = 1;
    g_srv.dirty = 1;
    return 0;
}

void win_tab_switch(Window *w, int idx)
{
    if (!w || w->ntabs <= 0)
        return;
    if (idx < 0) idx = 0;
    if (idx >= w->ntabs) idx = w->ntabs - 1;
    w->active_tab = idx;
    w->term = w->tabs[idx];
    int cols = g_srv.cellw > 0 ? w->cw / g_srv.cellw : 80;
    int rows = g_srv.cellh > 0 ? w->ch / g_srv.cellh : 24;
    term_resize(w->term, cols, rows);   /* casa com o tamanho atual da janela */
    w->term->dirty = 1;
    w->dirty = 1;
    g_srv.dirty = 1;
}

void win_tab_close(Window *w, int idx)
{
    if (!w || idx < 0 || idx >= w->ntabs)
        return;
    term_destroy(w->tabs[idx]);
    for (int i = idx; i < w->ntabs - 1; i++)
        w->tabs[i] = w->tabs[i + 1];
    w->ntabs--;
    w->tabs[w->ntabs] = NULL;
    if (w->ntabs == 0) {         /* fechou a ultima aba -> fecha a janela */
        w->term = NULL;
        dispd_close_window(w);
        return;
    }
    if (w->active_tab >= w->ntabs)
        w->active_tab = w->ntabs - 1;
    win_tab_switch(w, w->active_tab);
}

/* fecha uma janela por qualquer caminho (WM, builtin, atalho): destroi a
 * superficie, avisa o ntwm e — se for app — encerra o processo/conexao (#54) */
void dispd_close_window(Window *w)
{
    if (!w)
        return;
    unsigned id = w->id;
    int is_app = (w->kind == WK_APP);
    win_destroy(w);
    wmproto_ev_destroyed(id);
    if (is_app)
        appsrv_close_wid(id);
}

/* layout embutido (so quando nao ha ntwm): pilha vertical com margem. */
static void builtin_layout(void)
{
    Window *v[64];
    int n = 0;
    for (Window *w = g_srv.windows; w && n < 64; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws)
            v[n++] = w;
    if (n == 0)
        return;
    int g = 8;                                  /* margem */
    int x0 = g, y0 = g_srv.bar_h + g;
    int W = g_srv.scr_w - 2 * g;
    int H = g_srv.scr_h - g_srv.bar_h - 2 * g;
    if (W < 1) W = 1;
    if (H < 1) H = 1;
    int each = (H - (n - 1) * g) / n;
    if (each < 1) each = 1;
    for (int i = 0; i < n; i++) {
        Window *w = v[i];
        int top = y0 + i * (each + g);
        w->rect.left = x0;
        w->rect.top = top;
        w->rect.right = x0 + W;
        w->rect.bottom = top + each;
        w->z = i;
        int bp = w->border_px * 2;
        win_set_client_size(w, W - bp, each - bp - g_srv.title_h);
    }
}

/* remove ABAS cujo filho morreu (audit #63: antes so olhava a aba ATIVA -> uma
 * aba morta em background nao era removida, e ativa-la destruia a janela inteira
 * e todas as abas vivas). Fecha por aba; a ultima aba morta fecha a janela. */
static void reap_dead(void)
{
    unsigned wids[64];
    int nw = 0;
    for (Window *w = g_srv.windows; w && nw < 64; w = w->next)
        if (w->kind == WK_TERM)
            wids[nw++] = w->id;
    for (int k = 0; k < nw; k++) {
        Window *w = win_find(wids[k]);
        if (!w)
            continue;
        /* de tras pra frente: win_tab_close desloca as abas > idx */
        for (int i = w->ntabs - 1; i >= 0; i--) {
            if (i < w->ntabs && w->tabs[i] && !w->tabs[i]->alive) {
                win_tab_close(w, i);              /* ultima aba -> fecha a janela + evento */
                w = win_find(wids[k]);
                if (!w) {
                    dispd_log("terminal %u encerrado (ultima aba)", wids[k]);
                    break;
                }
                dispd_log("aba morta fechada (janela %u)", wids[k]);
            }
        }
    }
}

static void selftest_frame(void)
{
    struct { RECT r; COLORREF c; } rects[] = {
        {{ 0, 0, g_srv.scr_w, g_srv.scr_h }, DISP_BG },
        {{ 60, 60, 360, 260 }, RGB(220, 60, 60) },
        {{ 400, 120, 700, 320 }, RGB(60, 200, 90) },
        {{ 200, 360, 560, 560 }, RGB(70, 120, 240) },
    };
    for (int i = 0; i < 4; i++) {
        HBRUSH b = CreateSolidBrush(rects[i].c);
        FillRect(g_srv.cdc, &rects[i].r, b);
        DeleteObject(b);
    }
    const char *msg = "dispd selftest — GDI present OK (WS_POPUP + BitBlt)";
    SetBkMode(g_srv.cdc, TRANSPARENT);
    SetTextColor(g_srv.cdc, RGB(240, 240, 240));
    SelectObject(g_srv.cdc, g_srv.font);
    TextOutA(g_srv.cdc, 60, 20, msg, (int)strlen(msg));
    if (g_srv.present)
        g_srv.present->present(g_srv.present, &g_srv.frame);
}

int term_win_visible(Terminal *t)   /* #103: backends nao enxergam Window */
{
    return !t || !t->win || t->win->visible;
}

static int frame_tick(void)   /* retorna 1 se apresentou um quadro */
{
    if (g_srv.selftest) {
        selftest_frame();
        return 1;
    }
    wmproto_drain();
    appsrv_drain();
    input_process_keys();   /* roteia teclas enfileiradas pelo hook (#15) */
    reap_dead();
    if (!wmproto_connected())
        builtin_layout();

    /* transacao de layout aberta (FRAME-BEGIN sem COMMIT): nao apresenta um
     * quadro parcial (#10). Safety: se travar >60 ticks, DESCARTA o quadro
     * (nao publica parcial) e volta ao ultimo estado bom (#30). */
    static int frame_wait;
    if (g_srv.in_frame) {
        if (++frame_wait > 60) { wmproto_abort_frame(); frame_wait = 0; }
        return 0;
    }
    frame_wait = 0;

    /* idle-present: so recompoe quando algo muda (economiza CPU no WARP);
     * um tick periodico cobre relogio e piscar do cursor. So terminais
     * VISIVEIS forcam repaint — um terminal oculto que gera saida nao (#13). */
    static unsigned tick;
    if ((tick % 30) == 0) {
        g_srv.bar_dirty = 1;        /* relogio + titulo do focado (~2x/s) */
        vt_cursor_tick();           /* audit #115: alterna a fase do cursor... */
        if (g_srv.focused && g_srv.focused->term)
            g_srv.focused->term->dirty = 1;   /* ...e recompoe o focado p/ piscar */
    }
    if ((tick % 120) == 0)          /* #13: reinstala o hook periodicamente (~2s) */
        input_hook_refresh();
    tick++;
    int need = g_srv.dirty || g_srv.bar_dirty;
    if (!need && compositor_animating())   /* #35: mantem quadros enquanto anima */
        need = g_srv.dirty = 1;            /* recompoe cheio p/ o fade avancar */
    for (Window *w = g_srv.windows; w && !need; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws && w->term && w->term->dirty)
            need = 1;
    if (need)
        compose_and_present();      /* dono do clear de dirty/bar_dirty */
    return need;
}

static void focus_at(int x, int y)
{
    Window *w = win_at_point(x, y);
    if (w && w != g_srv.focused) {
        win_focus(w);
        wmproto_ev_focused(w);   /* mantem o ntwm em sincronia (#9) */
    }
}

static LRESULT CALLBACK root_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    /* raiz e' fullscreen em (0,0) -> coordenadas de cliente == tela */
    int mx = (short)LOWORD(lp), my = (short)HIWORD(lp);
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* fallback: SO quando o hook LL nao esta ativo (evita duplo). */
        if (!input_hook_active())
            input_key((unsigned)wp, (unsigned)((lp >> 16) & 0xff));
        return 0;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (!input_hook_active())
            input_keyup((unsigned)wp, (unsigned)((lp >> 16) & 0xff));   /* #13 */
        return 0;
    case WM_SYSCHAR:
        return 0;               /* engole o beep do Alt+tecla */
    case WM_LBUTTONDOWN: focus_at(mx, my); input_mouse(mx, my, 0, 1, 0); return 0;
    case WM_LBUTTONUP:   input_mouse(mx, my, 0, 0, 0); return 0;
    case WM_RBUTTONDOWN: focus_at(mx, my); input_mouse(mx, my, 2, 1, 0); return 0;
    case WM_RBUTTONUP:   input_mouse(mx, my, 2, 0, 0); return 0;
    case WM_MBUTTONDOWN: focus_at(mx, my); input_mouse(mx, my, 1, 1, 0); return 0;
    case WM_MBUTTONUP:   input_mouse(mx, my, 1, 0, 0); return 0;
    case WM_MOUSEWHEEL: {
        int up = GET_WHEEL_DELTA_WPARAM(wp) > 0;
        input_mouse(mx, my, up ? 64 : 65, 1, 0);   /* wheel como botao 64/65 */
        return 0;
    }
    case WM_MOUSEMOVE: {
        int btn = (wp & MK_LBUTTON) ? 0 : (wp & MK_MBUTTON) ? 1 :
                  (wp & MK_RBUTTON) ? 2 : -1;
        if (g_srv.ffm) focus_at(mx, my);
        input_mouse(mx, my, btn, 0, 1);   /* motion */
        return 0;
    }
    case WM_DISPLAYCHANGE: {   /* audit #85: resolucao/monitor mudou */
        int nw = LOWORD(lp), nh = HIWORD(lp);
        if (nw < 1) nw = GetSystemMetrics(SM_CXSCREEN);
        if (nh < 1) nh = GetSystemMetrics(SM_CYSCREEN);
        SetWindowPos(h, NULL, 0, 0, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
        compositor_resize(nw, nh);   /* recria backbuffer + present */
        wmproto_ev_output();         /* WM re-tila com a nova area */
        return 0;
    }
    case WM_CLOSE:
    case WM_DESTROY:
        g_srv.running = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(h, m, wp, lp);
}

/* forca a janela pra foreground+foco mesmo lancada por processo nao-interativo
 * (initd), via AttachThreadInput ao thread do foreground atual. */
static void force_foreground(HWND w)
{
    HWND fg = GetForegroundWindow();
    DWORD fgt = fg ? GetWindowThreadProcessId(fg, NULL) : 0;
    DWORD me = GetCurrentThreadId();
    if (fgt && fgt != me)
        AttachThreadInput(me, fgt, TRUE);
    BringWindowToTop(w);
    SetForegroundWindow(w);
    SetFocus(w);
    SetActiveWindow(w);
    if (fgt && fgt != me)
        AttachThreadInput(me, fgt, FALSE);
}

static HWND make_root(void)
{
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = root_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;    /* nos pintamos tudo */
    wc.lpszClassName = "ntudispd";
    RegisterClassA(&wc);

    /* area de trabalho: NAO topmost (janelas WK_FOREIGN ficam acima) e
     * NOACTIVATE (clicar num terminal nao traz o dispd pra frente cobrindo as
     * estrangeiras). O teclado vem do hook LL global, independe de foco. */
    return CreateWindowExA(WS_EX_NOACTIVATE, "ntudispd", "",
                           WS_POPUP | WS_VISIBLE,
                           0, 0, g_srv.scr_w, g_srv.scr_h,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

int main(void)
{
    g_srv.scr_w = GetSystemMetrics(SM_CXSCREEN);
    g_srv.scr_h = GetSystemMetrics(SM_CYSCREEN);
    if (g_srv.scr_w < 1) g_srv.scr_w = 1024;
    if (g_srv.scr_h < 1) g_srv.scr_h = 768;
    g_srv.cur_ws = 0;
    g_srv.next_id = 0;

    char bk[8] = "";
    GetEnvironmentVariableA("DISPD_SELFTEST", bk, sizeof bk);
    g_srv.selftest = (bk[0] == '1');

    /* os terminais filhos herdam o ambiente do dispd: anuncia um TERM decente
     * pra que o shell habilite cores/edicao de linha. */
    if (!GetEnvironmentVariableA("TERM", bk, sizeof bk) || !bk[0])
        SetEnvironmentVariableA("TERM", "xterm-256color");

    g_srv.root = make_root();
    if (!g_srv.root) {
        dispd_log("nao consegui criar a janela raiz (%lu)", GetLastError());
        return 1;
    }
    ShowWindow(g_srv.root, SW_SHOW);
    force_foreground(g_srv.root);

    if (compositor_init() != 0) {   /* #95: sem backbuffer nao ha compositor */
        dispd_log("compositor_init falhou — abortando");
        return 1;
    }
    g_srv.bar_h = g_srv.cellh + 6;
    g_srv.title_h = g_srv.cellh + 4;   /* barra de titulo por janela */
    char nt[8] = "";
    GetEnvironmentVariableA("DISPD_NOTITLES", nt, sizeof nt);
    if (nt[0] == '1')
        g_srv.title_h = 0;

    char ffmv[8] = "";
    GetEnvironmentVariableA("DISPD_FFM", ffmv, sizeof ffmv);
    g_srv.ffm = (ffmv[0] == '1');

    /* backend de present: DISPD_BACKEND=gdi|dxgi (default gdi). Se o dxgi nao
     * inicializar (sem d3d11/GPU, WinPE fino), cai no gdi automaticamente. */
    char be[16] = "";
    GetEnvironmentVariableA("DISPD_BACKEND", be, sizeof be);
    g_srv.present = NULL;
    if (!_stricmp(be, "dxgi")) {
        PresentBackend *p = present_dxgi_create();
        if (p && p->init(p, g_srv.root, g_srv.scr_w, g_srv.scr_h) == 0)
            g_srv.present = p;
        else {
            if (p) p->destroy(p);
            dispd_log("dxgi indisponivel; caindo no gdi");
        }
    }
    if (!g_srv.present) {
        g_srv.present = present_gdi_create();
        if (!g_srv.present || g_srv.present->init(g_srv.present, g_srv.root,
                                                  g_srv.scr_w, g_srv.scr_h) != 0) {
            dispd_log("present backend falhou");
            return 1;
        }
    }
    dispd_log("dispd %s iniciado (%dx%d, backend %s, selftest=%d)",
              NTU_VERSION, g_srv.scr_w, g_srv.scr_h,
              g_srv.present->name, g_srv.selftest);

    g_srv.running = 1;

    if (!g_srv.selftest) {
        input_install_hook();   /* teclado global, independe de foco */
        wmproto_start();
        appsrv_start();
        foreign_init();         /* WM das janelas nativas do Windows (WK_FOREIGN) */
        spawn_terminal(NULL);   /* algo na tela ja no boot */
    }

    while (g_srv.running) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                g_srv.running = 0;
                break;
            }
            DispatchMessageA(&msg);   /* sem TranslateMessage: teclas em root_proc */
        }
        int presented = frame_tick();
        int dxgi = g_srv.present && g_srv.present->name &&
                   !strcmp(g_srv.present->name, "dxgi");
        if (!(presented && dxgi))
            Sleep(16);   /* DXGI Present(1,0) ja pacea no vsync (#56) */
    }
    return 0;
}
