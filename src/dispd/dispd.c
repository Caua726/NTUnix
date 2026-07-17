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

Window *spawn_terminal(const char *cmdline)
{
    char def[MAX_PATH + 32], bb[MAX_PATH];
    if (!cmdline) {
        ntu_path("/system/bin/busybox.exe", bb, sizeof bb);
        snprintf(def, sizeof def, "\"%s\" ash -i", bb);
        cmdline = def;
    }

    int cols = 80, rows = 24;
    Window *w = win_create(WK_TERM, cols * g_srv.cellw, rows * g_srv.cellh);
    if (!w)
        return NULL;
    strncpy(w->title, "terminal", sizeof w->title - 1);
    w->term = term_create(cmdline, cols, rows, w);
    if (!w->term) {
        dispd_log("spawn_terminal: term_create falhou");
        win_destroy(w);
        return NULL;
    }
    win_focus(w);
    wmproto_ev_created(w);
    dispd_log("terminal criado (id %u, backend %s)", w->id,
              w->term->be ? w->term->be->name : "?");
    return w;
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

/* remove terminais cujo filho morreu */
static void reap_dead(void)
{
    unsigned dead[64];
    int nd = 0;
    for (Window *w = g_srv.windows; w && nd < 64; w = w->next)
        if (w->kind == WK_TERM && w->term && !w->term->alive)
            dead[nd++] = w->id;
    for (int i = 0; i < nd; i++) {
        Window *w = win_find(dead[i]);
        if (w) {
            win_destroy(w);
            wmproto_ev_destroyed(dead[i]);
            dispd_log("terminal %u encerrado", dead[i]);
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

static void frame_tick(void)
{
    if (g_srv.selftest) {
        selftest_frame();
        return;
    }
    wmproto_drain();
    appsrv_drain();
    reap_dead();
    if (!wmproto_connected())
        builtin_layout();

    /* idle-present: so recompoe quando algo muda (economiza CPU no WARP);
     * um tick periodico cobre relogio e piscar do cursor. */
    static unsigned tick;
    int need = g_srv.dirty;
    for (Window *w = g_srv.windows; w && !need; w = w->next)
        if (w->term && w->term->dirty)
            need = 1;
    if ((tick++ % 30) == 0)
        need = 1;
    if (need) {
        compose_and_present();
        g_srv.dirty = 0;
    }
}

static LRESULT CALLBACK root_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        /* fallback do teclado: se o hook LL nao disparar. O hook suprime o
         * que trata, entao nao ha processamento duplo. scan = lp[16..23]. */
        input_key((unsigned)wp, (unsigned)((lp >> 16) & 0xff));
        return 0;
    case WM_SYSCHAR:
        return 0;               /* engole o beep do Alt+tecla */
    case WM_LBUTTONDOWN: {
        Window *w = win_at_point(LOWORD(lp), HIWORD(lp));
        if (w)
            win_focus(w);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (g_srv.ffm) {
            Window *w = win_at_point(LOWORD(lp), HIWORD(lp));
            if (w && w != g_srv.focused)
                win_focus(w);
        }
        return 0;
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

    return CreateWindowExA(WS_EX_TOPMOST, "ntudispd", "",
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

    g_srv.root = make_root();
    if (!g_srv.root) {
        dispd_log("nao consegui criar a janela raiz (%lu)", GetLastError());
        return 1;
    }
    ShowWindow(g_srv.root, SW_SHOW);
    force_foreground(g_srv.root);

    compositor_init();
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
        frame_tick();
        Sleep(16);                    /* ~60 fps */
    }
    return 0;
}
