/*
 * compositor.c — modelo de janela + composicao do quadro.
 *
 * Cada janela tem um DIB proprio (backing store do conteudo). O compositor
 * compoe as janelas visiveis do workspace atual num backbuffer DIB e chama
 * o present backend. Janelas WK_TERM tem o conteudo renderizado pelo vt.c.
 */
#include "dispd.h"

/* cria um DIB top-down 32bpp + memory DC com ele selecionado */
static int make_dib(int w, int h, HDC *out_dc, HBITMAP *out_bmp, void **out_bits)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;          /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc)
        return -1;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) {
        DeleteDC(dc);
        return -1;
    }
    SelectObject(dc, bmp);
    *out_dc = dc;
    *out_bmp = bmp;
    *out_bits = bits;
    return 0;
}

static void free_dib(HDC dc, HBITMAP bmp)
{
    if (dc) DeleteDC(dc);
    if (bmp) DeleteObject(bmp);
}

static void window_defaults(Window *w)
{
    w->ws = g_srv.cur_ws;
    w->visible = 1;
    w->border_px = 2;
    w->border_rgb = BORDER_NORMAL;
    w->focus_rgb = BORDER_FOCUS;
    w->opacity = 238;
    w->shadow = 1;
    w->corner_radius = 10;
    w->animate = 1;
    w->titlebar = 1;
    w->scene_layer = SCENE_WINDOWS;
    w->app_role = APP_ROLE_TOPLEVEL;
}

/* audit #85: recria o backbuffer no novo tamanho de tela (WM_DISPLAYCHANGE).
 * Cria o novo ANTES de soltar o antigo — se make_dib falhar, mantem o atual. */
int compositor_resize(int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (w == g_srv.scr_w && h == g_srv.scr_h)
        return 0;
    HDC ndc; HBITMAP ndib; void *nbits;
    if (make_dib(w, h, &ndc, &ndib, &nbits) != 0) {
        dispd_log("compositor: resize do backbuffer falhou (%dx%d) — mantem %dx%d",
                  w, h, g_srv.scr_w, g_srv.scr_h);
        return -1;
    }
    free_dib(g_srv.cdc, g_srv.cdib);
    g_srv.cdc = ndc; g_srv.cdib = ndib; g_srv.cbits = nbits;
    g_srv.scr_w = w; g_srv.scr_h = h;
    g_srv.frame.memdc = ndc;
    g_srv.frame.dib = ndib;
    g_srv.frame.bgra = nbits;
    g_srv.frame.w = w; g_srv.frame.h = h;
    g_srv.frame.stride = w * 4;
    if (g_srv.present && g_srv.present->resize)
        g_srv.present->resize(g_srv.present, w, h);
    appsrv_reconfigure_layers();
    g_srv.dirty = 1;
    dispd_log("compositor: tela %dx%d", w, h);
    return 0;
}

int compositor_init(void)
{
    /* backbuffer composto do tamanho da tela — sem ele nao ha o que compor,
     * entao aborta a inicializacao em vez de seguir com DC nulo (#95). */
    if (make_dib(g_srv.scr_w, g_srv.scr_h, &g_srv.cdc, &g_srv.cdib, &g_srv.cbits) != 0) {
        dispd_log("compositor: backbuffer principal falhou (%dx%d)",
                  g_srv.scr_w, g_srv.scr_h);
        return -1;
    }

    g_srv.frame.memdc = g_srv.cdc;
    g_srv.frame.dib = g_srv.cdib;
    g_srv.frame.bgra = g_srv.cbits;
    g_srv.frame.w = g_srv.scr_w;
    g_srv.frame.h = g_srv.scr_h;
    g_srv.frame.stride = g_srv.scr_w * 4;

    /* fonte: TTF monoespacada com antialiasing (nivel kitty/WT), nao a bitmap
     * OEM dos anos 90. GDI substitui se a face faltar; OEM_FIXED_FONT de rede de
     * seguranca. Consolas -> Cascadia -> Lucida Console conforme disponivel. */
    {
        static const char *faces[] = { "Cascadia Mono", "Consolas", "Lucida Console", "" };
        for (int i = 0; faces[i][0]; i++) {
            g_srv.font = CreateFontA(-18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, faces[i]);
            if (g_srv.font)
                break;
        }
    }
    if (!g_srv.font)
        g_srv.font = (HFONT)GetStockObject(OEM_FIXED_FONT);
    g_srv.cellw = 9;
    g_srv.cellh = 18;
    HDC dc = CreateCompatibleDC(NULL);
    if (dc) {
        TEXTMETRICA tm;
        ZeroMemory(&tm, sizeof tm);
        HGDIOBJ of = SelectObject(dc, g_srv.font);
        if (GetTextMetricsA(dc, &tm)) {
            if (tm.tmAveCharWidth > 0) g_srv.cellw = tm.tmAveCharWidth;
            if (tm.tmHeight > 0)       g_srv.cellh = tm.tmHeight;
        }
        SelectObject(dc, of);
        DeleteDC(dc);
    }
    return 0;
}

Window *win_create(WinKind kind, int cw, int ch)
{
    Window *w = (Window *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = ++g_srv.next_id;
    w->kind = kind;
    window_defaults(w);
    if (cw < 1) cw = g_srv.cellw * 80;
    if (ch < 1) ch = g_srv.cellh * 24;
    w->cw = cw;
    w->ch = ch;
    if (make_dib(cw, ch, &w->memdc, &w->dib, &w->bits) != 0) {
        free(w);
        return NULL;
    }
    /* fundo inicial preto */
    RECT rc = { 0, 0, cw, ch };
    HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(w->memdc, &rc, b);
    DeleteObject(b);

    w->anim_ms = GetTickCount64();   /* fade-in ao aparecer */
    w->next = g_srv.windows;
    g_srv.windows = w;
    g_srv.dirty = 1;
    return w;
}

/* janela WK_APP: DIB sobre uma section compartilhada com o processo do app
 * (mesma memoria — o app desenha, o dispd compoe; analogo NT do wl_shm). */
Window *win_create_shared(int cw, int ch, HANDLE section)
{
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    Window *w = (Window *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = ++g_srv.next_id;
    w->kind = WK_APP;
    window_defaults(w);
    w->cw = cw;
    w->ch = ch;
    w->section = section;
    w->anim_ms = GetTickCount64();

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;             /* top-down */
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        free(w);
        return NULL;
    }
    w->dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &w->bits, section, 0);
    if (!w->dib) {
        DeleteDC(dc);
        free(w);
        return NULL;
    }
    SelectObject(dc, w->dib);
    w->memdc = dc;

    w->next = g_srv.windows;
    g_srv.windows = w;
    g_srv.dirty = 1;
    return w;
}

int win_replace_shared(Window *w, int cw, int ch, HANDLE section)
{
    if (!w || w->kind != WK_APP || !section || cw < 1 || ch < 1)
        return -1;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cw;
    bmi.bmiHeader.biHeight = -ch;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP dib = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS,
                                        &bits, section, 0) : NULL;
    if (!dc || !dib) {
        if (dc) DeleteDC(dc);
        return -1;
    }
    SelectObject(dc, dib);
    free_dib(w->memdc, w->dib);
    if (w->section)
        CloseHandle(w->section);
    w->memdc = dc;
    w->dib = dib;
    w->bits = bits;
    w->section = section;
    w->cw = cw;
    w->ch = ch;
    w->dirty = 1;
    g_srv.dirty = 1;
    return 0;
}

/* WK_FOREIGN: janela nativa do Windows que a gente gerencia (sem DIB nosso —
 * o win32k desenha; so cuidamos da geometria/foco/borda) */
Window *win_create_foreign(HWND hwnd)
{
    Window *w = (Window *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = ++g_srv.next_id;
    w->kind = WK_FOREIGN;
    window_defaults(w);
    w->hwnd = hwnd;
    w->animate = 0; /* Win32 recebe destino imediatamente; pixels nao sao compostos */
    w->next = g_srv.windows;
    g_srv.windows = w;
    g_srv.dirty = 1;
    return w;
}

void win_destroy(Window *w)
{
    if (!w)
        return;
    /* desliga da lista */
    Window **pp = &g_srv.windows;
    while (*pp && *pp != w)
        pp = &(*pp)->next;
    if (*pp)
        *pp = w->next;

    int refocus = (g_srv.focused == w);
    if (refocus)
        g_srv.focused = NULL;

    for (int i = 0; i < w->ntabs; i++)   /* destroi todas as abas */
        term_destroy(w->tabs[i]);
    w->ntabs = 0;
    w->term = NULL;
    if (w->kind == WK_FOREIGN)
        foreign_release(w);   /* restaura o estilo da janela do Windows */
    free_dib(w->memdc, w->dib);
    if (w->section)
        CloseHandle(w->section);
    free(w);

    /* #8: passa o foco pra outra janela do ws atual (com focused=1) */
    if (refocus) {
        Window *nf = NULL;
        for (Window *o = g_srv.windows; o; o = o->next)
            if (o->ws == g_srv.cur_ws) { nf = o; break; }
        win_focus(nf);
    }
    g_srv.dirty = 1;
}

Window *win_find(unsigned id)
{
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->id == id)
            return w;
    return NULL;
}

void win_set_client_size(Window *w, int cw, int ch)
{
    if (w->kind == WK_APP)
        return;            /* app: DIB e' fixo (sobre a section compartilhada) */
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (cw == w->cw && ch == w->ch)
        return;
    HDC ndc;
    HBITMAP nbmp;
    void *nbits;
    if (make_dib(cw, ch, &ndc, &nbmp, &nbits) != 0)
        return;
    free_dib(w->memdc, w->dib);
    w->memdc = ndc;
    w->dib = nbmp;
    w->bits = nbits;
    w->cw = cw;
    w->ch = ch;
    RECT rc = { 0, 0, cw, ch };
    HBRUSH b = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(w->memdc, &rc, b);
    DeleteObject(b);

    {
        int cols = g_srv.cellw > 0 ? cw / g_srv.cellw : 80;
        int rows = g_srv.cellh > 0 ? ch / g_srv.cellh : 24;
        for (int i = 0; i < w->ntabs; i++)   /* todas as abas acompanham */
            term_resize(w->tabs[i], cols, rows);
    }
    w->dirty = 1;
    g_srv.dirty = 1;
}

static int rect_empty(const RECT *r)
{
    return r->right <= r->left || r->bottom <= r->top;
}

/* Atualiza o destino logico imediatamente e inicia apenas a geometria visual.
 * Hit-test, foco, resize do cliente e WK_FOREIGN usam `rect`, nunca `visual`. */
void win_set_logical_rect(Window *w, const RECT *goal)
{
    if (!w || !goal)
        return;
    RECT old = w->rect;
    w->rect = *goal;
    if (w->kind == WK_FOREIGN || g_srv.interactive_wid == w->id ||
        !g_srv.animations || !w->animate) {
        w->visual.begin = w->visual.current = w->visual.goal = *goal;
        w->visual.active = 0;
        g_srv.dirty = 1;
        return;
    }
    RECT current = w->visual.current;
    if (rect_empty(&current)) {
        current = *goal;
        current.top += 8;
        current.bottom += 8;
    } else if (rect_empty(&old)) {
        current = *goal;
    }
    w->visual.begin = current;
    w->visual.current = current;
    w->visual.goal = *goal;
    w->visual.start_ms = GetTickCount64();
    w->visual.duration_ms = g_srv.move_ms > 0 ? g_srv.move_ms : 180;
    w->visual.active = memcmp(&current, goal, sizeof current) != 0;
    g_srv.dirty = 1;
}

int compositor_geometry_selftest(void)
{
    Window w;
    ZeroMemory(&w, sizeof w);
    w.id = 42;
    w.kind = WK_APP;
    w.animate = 1;
    RECT old = { 10, 20, 210, 140 };
    RECT goal = { 50, 70, 370, 250 };
    w.rect = old;
    w.visual.begin = w.visual.current = w.visual.goal = old;
    g_srv.animations = 1;
    g_srv.move_ms = 180;

    g_srv.interactive_wid = w.id;
    win_set_logical_rect(&w, &goal);
    if (w.visual.active || memcmp(&w.visual.current, &goal, sizeof goal) ||
        memcmp(&w.rect, &goal, sizeof goal))
        return 1;

    g_srv.interactive_wid = 0;
    w.rect = old;
    w.visual.begin = w.visual.current = w.visual.goal = old;
    win_set_logical_rect(&w, &goal);
    if (!w.visual.active || memcmp(&w.visual.current, &old, sizeof old) ||
        memcmp(&w.visual.goal, &goal, sizeof goal))
        return 2;
    return 0;
}

void win_focus(Window *w)
{
    /* audit #7: nao aceita foco em janela invisivel ou de outro workspace — o WM
     * pode mandar FOCUS de uma janela oculta ao remover a ultima do ws atual, e
     * o teclado iria pra uma janela que o usuario nao ve. Foco nulo nesse caso. */
    if (w && (!w->visible ||
              (w->scene_layer != SCENE_LAYERS && w->ws != g_srv.cur_ws)))
        w = NULL;
    if (g_srv.focused != w) {   /* #35: anima a transicao de borda (foco muda) */
        if (g_srv.focused) g_srv.focused->focus_ms = GetTickCount64();
        if (w) w->focus_ms = GetTickCount64();
    }
    for (Window *o = g_srv.windows; o; o = o->next)
        o->focused = 0;
    if (w) {
        w->focused = 1;
        g_srv.focused = w;
        if (w->kind == WK_FOREIGN)
            foreign_focus(w);   /* traz a janela do Windows pra frente */
    } else {
        g_srv.focused = NULL;   /* #7: limpa o ponteiro tambem */
    }
    g_srv.dirty = 1;
}

Window *win_at_point(int x, int y)
{
    Window *hit = NULL;
    for (Window *w = g_srv.windows; w; w = w->next) {
        if (!w->visible ||
            (w->scene_layer != SCENE_LAYERS && w->ws != g_srv.cur_ws))
            continue;
        if (x >= w->rect.left && x < w->rect.right &&
            y >= w->rect.top && y < w->rect.bottom) {
            if (!hit || w->z >= hit->z)
                hit = w;
        }
    }
    return hit;
}

void compositor_set_animations(int enabled, int move_ms, int open_ms,
                               int workspace_ms, int focus_ms)
{
    g_srv.animations = enabled != 0;
    g_srv.move_ms = move_ms < 0 ? 0 : (move_ms > 5000 ? 5000 : move_ms);
    g_srv.open_ms = open_ms < 0 ? 0 : (open_ms > 5000 ? 5000 : open_ms);
    g_srv.workspace_ms = workspace_ms < 0 ? 0 :
                         (workspace_ms > 5000 ? 5000 : workspace_ms);
    g_srv.focus_ms = focus_ms < 0 ? 0 : (focus_ms > 5000 ? 5000 : focus_ms);
}

void compositor_set_workspace(int ws)
{
    if (ws < 0 || ws >= NTUWM_WS || ws == g_srv.cur_ws)
        return;
    g_srv.old_ws = g_srv.cur_ws;
    g_srv.workspace_anim_dir = ws > g_srv.cur_ws ? 1 : -1;
    g_srv.cur_ws = ws; /* estado logico/input muda sem esperar a animacao */
    if (g_srv.animations && g_srv.workspace_ms > 0)
        g_srv.workspace_anim_ms = GetTickCount64();
    else
        g_srv.workspace_anim_ms = 0;
    g_srv.dirty = 1;
}

void compositor_recompute_workarea(void)
{
    int top = 0;
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->visible && w->scene_layer == SCENE_LAYERS &&
            (w->anchors & NTUAPP_ANCHOR_TOP) && w->exclusive_zone > top)
            top = w->exclusive_zone;
    if (top != g_srv.bar_h) {
        g_srv.bar_h = top;
        g_srv.dirty = 1;
        wmproto_ev_output();
    }
}

/* propaga titulo novo do terminal p/ a janela e emite evento (main thread) */
static void pump_title(Window *w)
{
    if (!w->term)
        return;
    EnterCriticalSection(&w->term->lock);
    int changed = w->term->title_changed;
    if (changed) {
        strncpy(w->title, w->term->title, sizeof w->title - 1);
        w->title[sizeof w->title - 1] = 0;
        w->term->title_changed = 0;
    }
    LeaveCriticalSection(&w->term->lock);
    if (changed) {
        for (char *p = w->title; *p; p++)   /* #17: sem controle -> sem injecao */
            if ((unsigned char)*p < 0x20) *p = ' ';
        wmproto_ev_title(w);
    }
}

/* audit #89: titulos vem em UTF-8 (OSC do terminal / GetWindowText); DrawTextA os
 * trata como ANSI -> mojibake em nao-ASCII. Converte pra UTF-16 e usa DrawTextW. */
static void draw_text_utf8(HDC dc, const char *s, RECT *rc, UINT fmt)
{
    WCHAR w[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 512);
    if (n > 0)
        DrawTextW(dc, w, n - 1, rc, fmt);   /* n inclui o NUL terminador */
}

/* barra de status no topo: workspaces + titulo focado + relogio.
 * Desenhada pelo dispd (dono da fonte/GDI); estilo dwm drawbar. */
/* #3: faixa x de cada workspace desenhada na barra (pra hit-test do clique) */
static int g_ws_x0[10], g_ws_x1[10];

int bar_ws_at(int x, int y)
{
    if (!g_srv.internal_bar || y < 0 || y >= g_srv.bar_h)
        return -1;
    for (int i = 0; i < 9; i++)
        if (g_ws_x1[i] > g_ws_x0[i] && x >= g_ws_x0[i] && x < g_ws_x1[i])
            return i;
    return -1;
}

static void draw_bar(void)
{
    if (g_srv.bar_h <= 0)
        return;
    HDC dc = g_srv.cdc;
    RECT bar = { 0, 0, g_srv.scr_w, g_srv.bar_h };
    HBRUSH bb = CreateSolidBrush(RGB(16, 16, 20));
    FillRect(dc, &bar, bb);
    DeleteObject(bb);

    /* quais workspaces tem janela */
    int occ[10] = { 0 };
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->ws >= 0 && w->ws < 10)
            occ[w->ws] = 1;

    HFONT of = (HFONT)SelectObject(dc, g_srv.font);
    SetBkMode(dc, TRANSPARENT);
    int pad = g_srv.cellw;
    int ty = (g_srv.bar_h - g_srv.cellh) / 2;
    int x = pad / 2;

    for (int i = 0; i < 9; i++) {
        char lbl[8];
        int n = snprintf(lbl, sizeof lbl, " %d ", i + 1);
        SIZE sz;
        GetTextExtentPoint32A(dc, lbl, n, &sz);
        if (i == g_srv.cur_ws) {
            RECT hl = { x, 0, x + sz.cx, g_srv.bar_h };
            HBRUSH hb = CreateSolidBrush(BORDER_FOCUS);
            FillRect(dc, &hl, hb);
            DeleteObject(hb);
            SetTextColor(dc, RGB(10, 10, 12));
        } else {
            SetTextColor(dc, occ[i] ? RGB(210, 210, 220) : RGB(90, 90, 100));
        }
        TextOutA(dc, x, ty, lbl, n);
        g_ws_x0[i] = x; g_ws_x1[i] = x + sz.cx;   /* #3: faixa clicavel */
        x += sz.cx;
    }

    /* relogio (direita) — computa primeiro pra limitar o titulo. k:<teclas> e
     * backend so em DISPD_DEBUG (#36). */
    char tm[32], ts[80];
    ntu_now(tm, sizeof tm);
    if (g_srv.debug) {
        const char *be = (g_srv.focused && g_srv.focused->term &&
                          g_srv.focused->term->be)
                             ? g_srv.focused->term->be->name : "-";
        snprintf(ts, sizeof ts, "k:%ld %s  %s", g_srv.keys_seen, be, tm);
    } else {
        snprintf(ts, sizeof ts, "%s", tm);
    }
    SIZE cs;
    GetTextExtentPoint32A(dc, ts, (int)strlen(ts), &cs);
    int clock_x = g_srv.scr_w - cs.cx - pad / 2;

    /* titulo do focado, clipado antes do relogio (#36 sem atravessar) */
    x += pad;
    if (g_srv.focused && g_srv.focused->title[0] && clock_x - x > 20) {
        SetTextColor(dc, RGB(200, 200, 210));
        RECT trc = { x, 0, clock_x - pad, g_srv.bar_h };
        draw_text_utf8(dc, g_srv.focused->title, &trc,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SetTextColor(dc, RGB(150, 150, 170));
    TextOutA(dc, clock_x, ty, ts, (int)strlen(ts));

    SelectObject(dc, of);
}

/* ---- glass: wallpaper com gradiente + composicao translucida ---- */

/* opacidade do conteudo (0..255); DISPD_OPACITY em % (default 92) */
static int glass_opacity(Window *w)
{
    return w->opacity < 0 ? 0 : (w->opacity > 255 ? 255 : w->opacity);
}

/* ---- frosted glass: blur do fundo atras das janelas translucidas ----
 * modernizacao (picom/hyprland fazem box-blur separavel do backbuffer atras da
 * superficie translucida). DISPD_BLUR=0 desliga; senao e' o raio (default 5). */
static int blur_radius(void)
{
    static int r = -1;
    if (r < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("DISPD_BLUR", v, sizeof v);
        if (v[0]) { r = atoi(v); if (r < 0) r = 0; if (r > 40) r = 40; }
        else r = 5;
    }
    return g_srv.quality_level >= 1 ? 0 : r;
}

static unsigned char *g_blur_scratch;
static size_t g_blur_scratch_cap;

/* box blur separavel (sliding-window, O(area) independe do raio) do retangulo
 * [x0,x1) x [y0,y1) do backbuffer, IN-PLACE. So os canais BGR. */
static void blur_bg(unsigned char *dst, int stride, int x0, int y0, int x1, int y1, int r)
{
    int w = x1 - x0, h = y1 - y0;
    if (r < 1 || w < 1 || h < 1)
        return;
    size_t need = (size_t)w * h * 4;
    if (need > g_blur_scratch_cap) {
        unsigned char *ns = (unsigned char *)realloc(g_blur_scratch, need);
        if (!ns) return;
        g_blur_scratch = ns;
        g_blur_scratch_cap = need;
    }
    unsigned char *tmp = g_blur_scratch;
    int tstride = w * 4;
    /* horizontal: dst -> tmp */
    for (int y = 0; y < h; y++) {
        const unsigned char *drow = dst + (size_t)(y0 + y) * stride + (size_t)x0 * 4;
        unsigned char *trow = tmp + (size_t)y * tstride;
        int s0 = 0, s1 = 0, s2 = 0, win = 0;
        for (int x = 0; x <= r && x < w; x++) { s0 += drow[x*4]; s1 += drow[x*4+1]; s2 += drow[x*4+2]; win++; }
        for (int x = 0; x < w; x++) {
            trow[x*4] = (unsigned char)(s0/win); trow[x*4+1] = (unsigned char)(s1/win); trow[x*4+2] = (unsigned char)(s2/win);
            int add = x + r + 1, sub = x - r;
            if (add < w) { s0 += drow[add*4]; s1 += drow[add*4+1]; s2 += drow[add*4+2]; win++; }
            if (sub >= 0) { s0 -= drow[sub*4]; s1 -= drow[sub*4+1]; s2 -= drow[sub*4+2]; win--; }
        }
    }
    /* vertical: tmp -> dst */
    for (int x = 0; x < w; x++) {
        int s0 = 0, s1 = 0, s2 = 0, win = 0;
        for (int y = 0; y <= r && y < h; y++) { const unsigned char *t = tmp + (size_t)y*tstride + x*4; s0 += t[0]; s1 += t[1]; s2 += t[2]; win++; }
        for (int y = 0; y < h; y++) {
            unsigned char *d = dst + (size_t)(y0 + y) * stride + (size_t)(x0 + x) * 4;
            d[0] = (unsigned char)(s0/win); d[1] = (unsigned char)(s1/win); d[2] = (unsigned char)(s2/win);
            int add = y + r + 1, sub = y - r;
            if (add < h) { const unsigned char *t = tmp + (size_t)add*tstride + x*4; s0 += t[0]; s1 += t[1]; s2 += t[2]; win++; }
            if (sub >= 0) { const unsigned char *t = tmp + (size_t)sub*tstride + x*4; s0 -= t[0]; s1 -= t[1]; s2 -= t[2]; win--; }
        }
    }
}

/* cor do wallpaper (gradiente vertical) na linha y, em BGR */
static void wallpaper_bgr(int y, int H, unsigned char *bgr)
{
    int tt = H > 1 ? (y * 255 / (H - 1)) : 0;
    bgr[0] = (unsigned char)(30 + (56 - 30) * tt / 255);
    bgr[1] = (unsigned char)(20 + (26 - 20) * tt / 255);
    bgr[2] = (unsigned char)(20 + (40 - 20) * tt / 255);
}

/* raio dos cantos arredondados; DISPD_ROUND=0 desliga (default 8) */
/* ---- animacoes (aparecer): fade-in por tempo (#35 modernizacao) ---- */
static int anim_enabled(void)
{
    return g_srv.animations;
}

/* alpha (0..255) da animacao de aparecer da janela; 255 = pronta. Zera anim_ms
 * ao terminar (ease-out cubico). */
static int win_anim_alpha(Window *w)
{
    if (!anim_enabled() || !w->animate || w->anim_ms == 0 ||
        g_srv.open_ms <= 0)
        return 255;
    ULONGLONG el = GetTickCount64() - w->anim_ms;
    if (el >= (ULONGLONG)g_srv.open_ms) { w->anim_ms = 0; return 255; }
    int u = 255 - (int)(el * 255 / g_srv.open_ms);   /* 255 -> 0 */
    return 255 - (int)((long long)u * u * u / (255 * 255));  /* 1-(1-t)^3 */
}

static COLORREF lerp_rgb(COLORREF a, COLORREF b, int t /*0..255*/)
{
    int ar = GetRValue(a), ag = GetGValue(a), ab = GetBValue(a);
    int br = GetRValue(b), bg = GetGValue(b), bb = GetBValue(b);
    return RGB(ar + (br - ar) * t / 255, ag + (bg - ag) * t / 255, ab + (bb - ab) * t / 255);
}

/* cor da borda com transicao de foco (#35): lerp entre a borda normal e a de
 * foco durante FOCUS_ANIM_MS apos a mudanca. Zera focus_ms ao terminar. */
static COLORREF border_color(Window *w)
{
    COLORREF target = w->focused ? w->focus_rgb : w->border_rgb;
    if (!anim_enabled() || !w->animate || w->focus_ms == 0 ||
        g_srv.focus_ms <= 0)
        return target;
    ULONGLONG el = GetTickCount64() - w->focus_ms;
    if (el >= (ULONGLONG)g_srv.focus_ms) { w->focus_ms = 0; return target; }
    COLORREF from = w->focused ? w->border_rgb : w->focus_rgb;
    return lerp_rgb(from, target, (int)(el * 255 / g_srv.focus_ms));
}

static LONG lerp_long(LONG a, LONG b, int t)
{
    return a + (LONG)(((long long)(b - a) * t) / 255);
}

/* Um tick atualiza todos os valores e conserva o goal logico separado. Damage
 * velho+novo e coberto pelo full frame usado enquanto ha animacao ativa. */
static void animation_tick(void)
{
    ULONGLONG now = GetTickCount64();
    for (Window *w = g_srv.windows; w; w = w->next) {
        AnimatedRect *a = &w->visual;
        if (!a->active)
            continue;
        ULONGLONG elapsed = now - a->start_ms;
        if (a->duration_ms <= 0 || elapsed >= (ULONGLONG)a->duration_ms) {
            a->current = a->goal;
            a->active = 0;
            continue;
        }
        int linear = (int)(elapsed * 255 / a->duration_ms);
        int inv = 255 - linear;
        int eased = 255 - (int)((long long)inv * inv * inv / (255 * 255));
        a->current.left = lerp_long(a->begin.left, a->goal.left, eased);
        a->current.top = lerp_long(a->begin.top, a->goal.top, eased);
        a->current.right = lerp_long(a->begin.right, a->goal.right, eased);
        a->current.bottom = lerp_long(a->begin.bottom, a->goal.bottom, eased);
    }
    if (g_srv.workspace_anim_ms &&
        now - g_srv.workspace_anim_ms >= (ULONGLONG)g_srv.workspace_ms)
        g_srv.workspace_anim_ms = 0;
}

static RECT window_draw_rect(Window *w)
{
    RECT r = rect_empty(&w->visual.current) ? w->rect : w->visual.current;
    if (w->scene_layer == SCENE_LAYERS || !g_srv.workspace_anim_ms)
        return r;
    ULONGLONG elapsed = GetTickCount64() - g_srv.workspace_anim_ms;
    int t = g_srv.workspace_ms > 0
        ? (int)(elapsed * 255 / g_srv.workspace_ms) : 255;
    if (t > 255) t = 255;
    int inv = 255 - t;
    int eased = 255 - (int)((long long)inv * inv * inv / (255 * 255));
    int offset = 0;
    if (w->ws == g_srv.cur_ws)
        offset = g_srv.workspace_anim_dir * g_srv.scr_w * (255 - eased) / 255;
    else if (w->ws == g_srv.old_ws)
        offset = -g_srv.workspace_anim_dir * g_srv.scr_w * eased / 255;
    r.left += offset;
    r.right += offset;
    return r;
}

int compositor_animating(void)
{
    if (g_srv.toast_ms != 0)      /* #2: toast ativo -> mantem quadros ate expirar */
        return 1;
    if (compositor_ghosts_active())  /* #103: fade-out de janela fechando */
        return 1;
    if (!anim_enabled())
        return 0;
    if (g_srv.workspace_anim_ms)
        return 1;
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->anim_ms != 0 || w->focus_ms != 0 || w->visual.active)
            return 1;
    return 0;
}

/* arredonda os 4 cantos da janela `outer`: os pixels do canto fora do arco de
 * raio `radius` viram wallpaper (recorta a moldura retangular). Limitado a clip. */
static void round_corners(const RECT *outer, int radius, const RECT *clip)
{
    if (radius < 1)
        return;
    unsigned char *p = (unsigned char *)g_srv.cbits;
    if (!p)
        return;
    int W = g_srv.scr_w, H = g_srv.scr_h, stride = g_srv.frame.stride, r2 = radius * radius;
    struct { int bx, by, cx, cy; } cn[4] = {
        { outer->left,           outer->top,            outer->left + radius,   outer->top + radius },
        { outer->right - radius, outer->top,            outer->right - radius,  outer->top + radius },
        { outer->left,           outer->bottom - radius, outer->left + radius,   outer->bottom - radius },
        { outer->right - radius, outer->bottom - radius, outer->right - radius,  outer->bottom - radius },
    };
    for (int k = 0; k < 4; k++) {
        for (int y = cn[k].by; y < cn[k].by + radius; y++) {
            if (y < 0 || y >= H) continue;
            if (clip && (y < clip->top || y >= clip->bottom)) continue;
            unsigned char bgr[3];
            wallpaper_bgr(y, H, bgr);
            unsigned char *row = p + (size_t)y * stride;
            for (int x = cn[k].bx; x < cn[k].bx + radius; x++) {
                if (x < 0 || x >= W) continue;
                if (clip && (x < clip->left || x >= clip->right)) continue;
                int dxp = x - cn[k].cx, dyp = y - cn[k].cy;
                if (dxp * dxp + dyp * dyp > r2) {   /* fora do arco -> wallpaper */
                    unsigned char *d = row + x * 4;
                    d[0] = bgr[0]; d[1] = bgr[1]; d[2] = bgr[2]; d[3] = 255;
                }
            }
        }
    }
}

/* damage tracking (deep-compositors §0.1: "sem damage voce redesenha a tela
 * inteira todo frame — inaceitavel no software"). DISPD_DAMAGE=0 desliga (volta
 * ao recompose-tela-cheia). So no backend GDI — o flip DXGI apresenta o buffer
 * inteiro de qualquer forma, entao sub-retangulo nao ajuda la. */
static int damage_enabled(void)
{
    static int en = -1;
    if (en < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("DISPD_DAMAGE", v, sizeof v);
        en = (v[0] == '0') ? 0 : 1;
    }
    if (en && g_srv.present && g_srv.present->name &&
        strcmp(g_srv.present->name, "gdi") != 0)
        return 0;
    return en;
}

static int rects_hit(const RECT *a, const RECT *b)
{
    return a->left < b->right && b->left < a->right &&
           a->top < b->bottom && b->top < a->bottom;
}

/* gradiente vertical (escuro-azulado -> roxo-escuro) num retangulo do backbuffer.
 * So [x0,x1) x [y0,y1) — o damage repinta so o que mudou. */
static void draw_wallpaper_rect(int x0, int y0, int x1, int y1)
{
    unsigned char *p = (unsigned char *)g_srv.cbits;
    int W = g_srv.scr_w, H = g_srv.scr_h, stride = g_srv.frame.stride;
    if (!p)
        return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    for (int y = y0; y < y1; y++) {
        int tt = H > 1 ? (y * 255 / (H - 1)) : 0;
        unsigned char r = (unsigned char)(20 + (40 - 20) * tt / 255);
        unsigned char g = (unsigned char)(20 + (26 - 20) * tt / 255);
        unsigned char b = (unsigned char)(30 + (56 - 30) * tt / 255);
        unsigned char *row = p + (size_t)y * stride;
        for (int x = x0; x < x1; x++) {
            row[x * 4 + 0] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
        }
    }
}

/* coleta janelas visiveis do ws atual, ordenadas por z crescente */
static int collect_visible(Window **vis, int cap)
{
    int nv = 0;
    for (Window *w = g_srv.windows; w && nv < cap; w = w->next)
        if (w->visible &&
            (w->scene_layer == SCENE_LAYERS || w->ws == g_srv.cur_ws ||
             (g_srv.workspace_anim_ms && w->ws == g_srv.old_ws)))
            vis[nv++] = w;
    for (int i = 1; i < nv; i++) {
        Window *k = vis[i];
        int j = i - 1;
        while (j >= 0 &&
               (vis[j]->scene_layer > k->scene_layer ||
                (vis[j]->scene_layer == k->scene_layer && vis[j]->z > k->z))) {
            vis[j + 1] = vis[j];
            j--;
        }
        vis[j + 1] = k;
    }
    return nv;
}

/* blenda o DIB da janela sobre o backbuffer (glass translucido), limitado a
 * `clip` (o retangulo de damage; NULL = sem limite). Opacidade cheia -> BitBlt
 * opaco (rapido). O src e' amostrado em (px-dx, py-dy). */
static void blit_glass(HDC memdc, const void *sbits, int sw,
                       int dx, int dy, int bw, int bh, const RECT *clip,
                       int amul, int opacity)
{
    int W = g_srv.scr_w, H = g_srv.scr_h;
    int x0 = dx, y0 = dy, x1 = dx + bw, y1 = dy + bh;
    if (clip) {
        if (x0 < clip->left)   x0 = clip->left;
        if (y0 < clip->top)    y0 = clip->top;
        if (x1 > clip->right)  x1 = clip->right;
        if (y1 > clip->bottom) y1 = clip->bottom;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > W) x1 = W;
    if (y1 > H) y1 = H;
    if (x0 >= x1 || y0 >= y1)
        return;

    int a = opacity;
    if (amul < 255) a = a * amul / 255;   /* #35: fade de aparecer (amul<255) */
    if (a >= 255) {   /* opaco: BitBlt so a interseccao */
        BitBlt(g_srv.cdc, x0, y0, x1 - x0, y1 - y0, memdc, x0 - dx, y0 - dy, SRCCOPY);
        return;
    }
    GdiFlush();   /* garante que o ExtTextOut do vt_render chegou ao DIB */
    unsigned char *dst = (unsigned char *)g_srv.cbits;
    const unsigned char *src = (const unsigned char *)sbits;
    if (!dst || !src) return;
    int dstride = g_srv.frame.stride, sstride = sw * 4;
    int ia = 255 - a;   /* a ja computado acima (com o amul da animacao) */
    /* frosted: borra o fundo (que ja tem wallpaper + janelas de baixo) antes de
     * misturar o conteudo por cima -> a parte translucida (ia) mostra o borrado */
    blur_bg(dst, dstride, x0, y0, x1, y1, blur_radius());
    for (int py = y0; py < y1; py++) {
        unsigned char *drow = dst + (size_t)py * dstride;
        const unsigned char *srow = src + (size_t)(py - dy) * sstride;
        for (int px = x0; px < x1; px++) {
            unsigned char *d = drow + px * 4;
            const unsigned char *s = srow + (px - dx) * 4;
            /* audit #88: /255 arredondado (o >>8 = /256 escurecia levemente) */
            unsigned b0 = s[0] * a + d[0] * ia;
            unsigned b1 = s[1] * a + d[1] * ia;
            unsigned b2 = s[2] * a + d[2] * ia;
            d[0] = (unsigned char)((b0 + 1 + (b0 >> 8)) >> 8);
            d[1] = (unsigned char)((b1 + 1 + (b1 >> 8)) >> 8);
            d[2] = (unsigned char)((b2 + 1 + (b2 >> 8)) >> 8);
        }
    }
}

/* BGRA premultiplicado: out = src + dst*(1-a). Usado pelo app protocol v2. */
static void blit_premultiplied(const void *sbits, int sw, int dx, int dy,
                               int bw, int bh, const RECT *clip, int opacity)
{
    unsigned char *dst = (unsigned char *)g_srv.cbits;
    const unsigned char *src = (const unsigned char *)sbits;
    if (!dst || !src)
        return;
    int x0 = dx, y0 = dy, x1 = dx + bw, y1 = dy + bh;
    if (clip) {
        if (x0 < clip->left) x0 = clip->left;
        if (y0 < clip->top) y0 = clip->top;
        if (x1 > clip->right) x1 = clip->right;
        if (y1 > clip->bottom) y1 = clip->bottom;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > g_srv.scr_w) x1 = g_srv.scr_w;
    if (y1 > g_srv.scr_h) y1 = g_srv.scr_h;
    for (int y = y0; y < y1; y++) {
        unsigned char *drow = dst + (size_t)y * g_srv.frame.stride;
        const unsigned char *srow = src + (size_t)(y - dy) * sw * 4;
        for (int x = x0; x < x1; x++) {
            unsigned char *d = drow + x * 4;
            const unsigned char *s = srow + (x - dx) * 4;
            int a = s[3] * opacity / 255;
            int ia = 255 - a;
            int sb = s[0] * opacity / 255;
            int sg = s[1] * opacity / 255;
            int sr = s[2] * opacity / 255;
            d[0] = (unsigned char)(sb + (d[0] * ia + 127) / 255);
            d[1] = (unsigned char)(sg + (d[1] * ia + 127) / 255);
            d[2] = (unsigned char)(sr + (d[2] * ia + 127) / 255);
            d[3] = 255;
        }
    }
}

/* Mascara 1D cacheada por raio. Evita recalcular o falloff da sombra por
 * pixel/frame; qualidade adaptativa reduz raio/softness antes de tocar layout. */
static unsigned char g_shadow_mask[33][33];
static unsigned char g_shadow_ready[33];

static unsigned char *shadow_mask(int radius)
{
    if (radius < 1) radius = 1;
    if (radius > 32) radius = 32;
    if (!g_shadow_ready[radius]) {
        for (int i = 0; i <= radius; i++) {
            int inv = radius - i;
            g_shadow_mask[radius][i] =
                (unsigned char)(70 * inv * inv / (radius * radius));
        }
        g_shadow_ready[radius] = 1;
    }
    return g_shadow_mask[radius];
}

static void draw_shadow(const RECT *r, const RECT *clip, int enabled)
{
    if (!enabled || g_srv.quality_level >= 3 || !g_srv.cbits)
        return;
    int radius = g_srv.quality_level >= 2 ? 4 : 12;
    unsigned char *mask = shadow_mask(radius);
    unsigned char *dst = (unsigned char *)g_srv.cbits;
    for (int y = r->top - radius; y < r->bottom + radius; y++) {
        if (y < 0 || y >= g_srv.scr_h ||
            (clip && (y < clip->top || y >= clip->bottom)))
            continue;
        for (int x = r->left - radius; x < r->right + radius; x++) {
            if (x < 0 || x >= g_srv.scr_w ||
                (clip && (x < clip->left || x >= clip->right)) ||
                (x >= r->left && x < r->right && y >= r->top && y < r->bottom))
                continue;
            int dx = x < r->left ? r->left - x : (x >= r->right ? x - r->right + 1 : 0);
            int dy = y < r->top ? r->top - y : (y >= r->bottom ? y - r->bottom + 1 : 0);
            int d = dx > dy ? dx : dy;
            if (d > radius) continue;
            int a = mask[d];
            unsigned char *p = dst + (size_t)y * g_srv.frame.stride + x * 4;
            p[0] = (unsigned char)(p[0] * (255 - a) / 255);
            p[1] = (unsigned char)(p[1] * (255 - a) / 255);
            p[2] = (unsigned char)(p[2] * (255 - a) / 255);
        }
    }
}

/* compoe UMA janela (borda + barra de abas + conteudo) dentro de `clip`. As
 * operacoes GDI ja saem clipadas pela regiao selecionada no cdc; o blit_glass
 * (loop de pixel manual) recebe o clip explicito. */
static void compose_one_window(Window *w, const RECT *clip)
{
    RECT vr = window_draw_rect(w);
    if (w->kind == WK_TERM && w->term && w->term->dirty)
        vt_render(w->term, w->memdc, g_srv.font, g_srv.cellw, g_srv.cellh);

    draw_shadow(&vr, clip, w->shadow && w->scene_layer == SCENE_WINDOWS);

    /* borda: MOLDURA (nao preenche o miolo -> o wallpaper aparece sob o
     * glass do terminal). foco = destaque */
    COLORREF bc = border_color(w);   /* #35: lerp na transicao de foco */
    HBRUSH bb = CreateSolidBrush(bc);
    int bp = w->border_px;
    RECT edges[4] = {
        { vr.left, vr.top, vr.right, vr.top + bp },
        { vr.left, vr.bottom - bp, vr.right, vr.bottom },
        { vr.left, vr.top, vr.left + bp, vr.bottom },
        { vr.right - bp, vr.top, vr.right, vr.bottom },
    };
    for (int e = 0; e < 4; e++)
        FillRect(g_srv.cdc, &edges[e], bb);
    DeleteObject(bb);

    int ix = vr.left + w->border_px;
    int iy = vr.top + w->border_px;
    int iw = (vr.right - vr.left) - 2 * w->border_px;
    int title_h = w->titlebar ? g_srv.title_h : 0;

    /* barra de ABAS do terminal (i3-ish); barra de titulo simples p/ apps */
    if (title_h > 0 && iw > 0) {
        SelectObject(g_srv.cdc, g_srv.font);
        SetBkMode(g_srv.cdc, TRANSPARENT);
        if (w->kind == WK_TERM && w->ntabs > 0) {
            int tw = iw / w->ntabs;
            if (tw < 1) tw = 1;
            for (int ti = 0; ti < w->ntabs; ti++) {
                int tx = ix + ti * tw;
                int twid = (ti == w->ntabs - 1) ? (ix + iw - tx) : tw;
                int act = (ti == w->active_tab);
                RECT tb = { tx, iy, tx + twid, iy + title_h };
                COLORREF tc = act ? (w->focused ? BORDER_FOCUS : RGB(70, 70, 84))
                                  : RGB(30, 30, 38);
                HBRUSH tbb = CreateSolidBrush(tc);
                FillRect(g_srv.cdc, &tb, tbb);
                DeleteObject(tbb);
                Terminal *tt = w->tabs[ti];
                char base[256] = "sh";
                if (tt) {   /* audit #68: copia o titulo SOB o lock do terminal — o
                             * reader escreve OSC concorrente; ler tt->title solto
                             * era data race */
                    EnterCriticalSection(&tt->lock);
                    if (tt->title[0]) {
                        strncpy(base, tt->title, sizeof base - 1);
                        base[sizeof base - 1] = 0;
                    }
                    LeaveCriticalSection(&tt->lock);
                }
                char lbl[288];
                snprintf(lbl, sizeof lbl, " %d %s", ti + 1, base);
                SetTextColor(g_srv.cdc, act
                    ? (w->focused ? RGB(12, 14, 20) : RGB(225, 225, 235))
                    : RGB(150, 150, 165));
                RECT tr = { tx + 4, iy, tx + twid - 4, iy + title_h };
                draw_text_utf8(g_srv.cdc, lbl, &tr,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        } else {
            RECT tb = { ix, iy, ix + iw, iy + title_h };
            COLORREF tc = w->focused ? BORDER_FOCUS : RGB(44, 44, 52);
            HBRUSH tbb = CreateSolidBrush(tc);
            FillRect(g_srv.cdc, &tb, tbb);
            DeleteObject(tbb);
            SetTextColor(g_srv.cdc, w->focused ? RGB(12, 14, 20)
                                               : RGB(180, 180, 195));
            const char *base = w->title[0] ? w->title : "app";
            RECT tr = { ix + 6, iy, ix + iw - 6, iy + title_h };
            draw_text_utf8(g_srv.cdc, base, &tr,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    /* conteudo (terminal/app) abaixo da barra de titulo, SEMPRE clipado a
     * area cliente: uma surface de app maior que o tile nao pode invadir a
     * janela vizinha nem sobrar fundo de borda (#49) */
    int ih = (vr.bottom - vr.top) - 2 * w->border_px - title_h;
    int bw = w->cw < iw ? w->cw : iw;
    int bh = w->ch < ih ? w->ch : ih;
    if (bw > 0 && bh > 0) {
        if (w->kind == WK_TERM)          /* glass: terminal translucido */
            blit_glass(w->memdc, w->bits, w->cw, ix, iy + title_h, bw, bh, clip,
                       win_anim_alpha(w), glass_opacity(w));
        else if (w->premultiplied)
            blit_premultiplied(w->bits, w->cw, ix, iy + title_h, bw, bh,
                               clip, w->opacity);
        else if (w->opacity < 255)
            blit_glass(w->memdc, w->bits, w->cw, ix, iy + title_h, bw, bh,
                       clip, win_anim_alpha(w), w->opacity);
        else
            BitBlt(g_srv.cdc, ix, iy + title_h, bw, bh, w->memdc, 0, 0, SRCCOPY);
    }

    /* cantos arredondados: recorta a moldura DEPOIS de tudo desenhado (garante
     * que o GDI da borda chegou ao DIB antes de sobrescrever os cantos) */
    if (w->corner_radius > 0) {
        GdiFlush();
        round_corners(&vr, w->corner_radius, clip);
    }
}

/* recompoe uma regiao do quadro: wallpaper + janelas que cruzam `r` (em ordem z)
 * + barra, tudo clipado a `r`. Base do damage tracking. */
/* deep-compositors #11 (occlusion): uma janela e' TOTALMENTE opaca (nao deixa ver
 * o que esta atras) so se app (BitBlt opaco) ou glass 100%, SEM cantos
 * arredondados (que cortam buracos) e SEM animacao de aparecer. Com os defaults
 * modernos (glass translucido + cantos), quase nunca ocorre — mas e' correto p/
 * o caso de um app opaco cobrindo outra janela. */
static int win_opaque(Window *w)
{
    if (w->corner_radius > 0 || w->anim_ms != 0 || w->opacity < 255)
        return 0;
    if (w->kind == WK_APP && !w->premultiplied)
        return 1;
    return w->kind == WK_TERM && glass_opacity(w) >= 255;
}

static int rect_contains(const RECT *a, const RECT *b)
{
    return a->left <= b->left && a->top <= b->top &&
           a->right >= b->right && a->bottom >= b->bottom;
}

static void compose_region(const RECT *r)
{
    draw_wallpaper_rect(r->left, r->top, r->right, r->bottom);

    HRGN rgn = CreateRectRgn(r->left, r->top, r->right, r->bottom);
    SelectClipRgn(g_srv.cdc, rgn);

    Window *vis[256];
    int nv = collect_visible(vis, 256);
    for (int i = 0; i < nv; i++) {
        RECT ir = window_draw_rect(vis[i]);
        if (!rects_hit(&ir, r))
            continue;
        int occluded = 0;   /* pula se uma janela OPACA acima cobre esta inteira */
        for (int j = i + 1; j < nv && !occluded; j++) {
            RECT jr = window_draw_rect(vis[j]);
            if (win_opaque(vis[j]) && rect_contains(&jr, &ir))
                occluded = 1;
        }
        if (!occluded)
            compose_one_window(vis[i], r);
    }

    RECT bar = { 0, 0, g_srv.scr_w, g_srv.bar_h };
    if (g_srv.internal_bar && g_srv.bar_h > 0 && rects_hit(&bar, r))
        draw_bar();

    SelectClipRgn(g_srv.cdc, NULL);
    DeleteObject(rgn);
}

/* audit #2: toast — overlay transitorio no topo-centro (erro/feedback). Enquanto
 * ativo, compositor_animating mantem o full-recompose, entao so desenho no caminho
 * cheio. */
#define TOAST_MS 3000
static void draw_toast(void)
{
    if (!g_srv.toast[0] || g_srv.toast_ms == 0)
        return;
    if (GetTickCount64() - g_srv.toast_ms >= TOAST_MS) { g_srv.toast_ms = 0; return; }
    HDC dc = g_srv.cdc;
    HFONT of = (HFONT)SelectObject(dc, g_srv.font);
    RECT tr = { 0, 0, 4000, 200 };
    draw_text_utf8(dc, g_srv.toast, &tr, DT_CALCRECT | DT_SINGLELINE);
    int tw = tr.right - tr.left, th = tr.bottom - tr.top;
    int padx = 18, pady = 9, bw = tw + 2 * padx, bh = th + 2 * pady;
    int bx = (g_srv.scr_w - bw) / 2, by = g_srv.bar_h + 14;
    HBRUSH bg = CreateSolidBrush(RGB(28, 30, 42));
    HPEN pen = CreatePen(PS_SOLID, 2, BORDER_FOCUS);
    HBRUSH ob = (HBRUSH)SelectObject(dc, bg);
    HPEN op = (HPEN)SelectObject(dc, pen);
    RoundRect(dc, bx, by, bx + bw, by + bh, 12, 12);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(bg); DeleteObject(pen);
    SetTextColor(dc, RGB(235, 235, 245));
    SetBkMode(dc, TRANSPARENT);
    RECT txt = { bx + padx, by + pady, bx + bw - padx, by + bh - pady };
    draw_text_utf8(dc, g_srv.toast, &txt, DT_SINGLELINE | DT_LEFT | DT_VCENTER);
    SelectObject(dc, of);
}

/* audit #103: fade-out ao fechar via GHOST — copia a aparencia da janela do
 * backbuffer ANTES de destrui-la (lifecycle inalterado) e faz fade por cima do
 * layout re-tilado. Seguro: nao mexe no ciclo de vida da janela. */
#define GHOST_MS 170
#define MAX_GHOSTS 6
typedef struct { void *bits; int w, h; RECT rect; ULONGLONG ms; } Ghost;
static Ghost g_ghosts[MAX_GHOSTS];

void compositor_ghost_capture(const RECT *r)
{
    if (!anim_enabled() || !g_srv.cbits)
        return;
    int w = r->right - r->left, h = r->bottom - r->top;
    if (w < 1 || h < 1 || w > 4096 || h > 4096)
        return;
    Ghost *g = NULL;
    for (int i = 0; i < MAX_GHOSTS; i++)
        if (g_ghosts[i].ms == 0) { g = &g_ghosts[i]; break; }
    if (!g) return;
    g->bits = malloc((size_t)w * h * 4);
    if (!g->bits) return;
    const unsigned char *src = (const unsigned char *)g_srv.cbits;
    int stride = g_srv.frame.stride;
    for (int y = 0; y < h; y++) {
        int sy = r->top + y;
        if (sy < 0 || sy >= g_srv.scr_h) continue;
        memcpy((char *)g->bits + (size_t)y * w * 4,
               src + (size_t)sy * stride + (size_t)r->left * 4, (size_t)w * 4);
    }
    g->w = w; g->h = h; g->rect = *r; g->ms = GetTickCount64();
}

static void draw_ghosts(void)
{
    unsigned char *dst = (unsigned char *)g_srv.cbits;
    if (!dst) return;
    int stride = g_srv.frame.stride, W = g_srv.scr_w, H = g_srv.scr_h;
    for (int i = 0; i < MAX_GHOSTS; i++) {
        Ghost *g = &g_ghosts[i];
        if (g->ms == 0) continue;
        ULONGLONG el = GetTickCount64() - g->ms;
        if (el >= GHOST_MS) { free(g->bits); g->bits = NULL; g->ms = 0; continue; }
        int a = 255 - (int)(el * 255 / GHOST_MS), ia = 255 - a;   /* fade out */
        const unsigned char *gb = (const unsigned char *)g->bits;
        for (int y = 0; y < g->h; y++) {
            int dy = g->rect.top + y;
            if (dy < 0 || dy >= H) continue;
            unsigned char *drow = dst + (size_t)dy * stride;
            const unsigned char *grow = gb + (size_t)y * g->w * 4;
            for (int x = 0; x < g->w; x++) {
                int dxp = g->rect.left + x;
                if (dxp < 0 || dxp >= W) continue;
                unsigned char *d = drow + dxp * 4;
                const unsigned char *s = grow + x * 4;
                d[0] = (unsigned char)((s[0]*a + d[0]*ia) / 255);
                d[1] = (unsigned char)((s[1]*a + d[1]*ia) / 255);
                d[2] = (unsigned char)((s[2]*a + d[2]*ia) / 255);
            }
        }
    }
}

int compositor_ghosts_active(void)
{
    for (int i = 0; i < MAX_GHOSTS; i++)
        if (g_ghosts[i].ms != 0) return 1;
    return 0;
}

/* apresenta o quadro; sub != NULL -> so o sub-retangulo (damage) */
static void present_frame(const RECT *sub)
{
    if (sub) {
        g_srv.frame.dirty_x = sub->left;
        g_srv.frame.dirty_y = sub->top;
        g_srv.frame.dirty_w = sub->right - sub->left;
        g_srv.frame.dirty_h = sub->bottom - sub->top;
    } else {
        g_srv.frame.dirty_w = 0;   /* quadro inteiro */
    }
    if (g_srv.present)
        g_srv.present->present(g_srv.present, &g_srv.frame);
}

void compose_and_present(void)
{
    ULONGLONG compose_start = GetTickCount64();
    animation_tick();
    /* propaga titulos de todas as visiveis (eventos independem do damage) */
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws)
            pump_title(w);

    /* recompose TOTAL: qualquer mudanca estrutural (layout/foco/criar/destruir/
     * app-commit) seta g_srv.dirty -> repinta a tela inteira (fallback seguro). */
    if (g_srv.dirty || !damage_enabled()) {
        RECT full = { 0, 0, g_srv.scr_w, g_srv.scr_h };
        compose_region(&full);
        draw_ghosts();           /* #103: fade-out das janelas fechando */
        draw_toast();            /* #2: overlay de feedback por cima de tudo */
        present_frame(NULL);
        g_srv.dirty = 0;
        g_srv.bar_dirty = 0;
        ULONGLONG elapsed = GetTickCount64() - compose_start;
        static int good_frames;
        if (elapsed > 18) {
            good_frames = 0;
            if (++g_srv.slow_frames >= 8 && g_srv.quality_level < 3) {
                g_srv.quality_level++;
                g_srv.slow_frames = 0;
                dispd_log("compositor: qualidade adaptativa -> nivel %d",
                          g_srv.quality_level);
            }
        } else {
            if (g_srv.slow_frames > 0) g_srv.slow_frames--;
            if (++good_frames >= 240 && g_srv.quality_level > 0) {
                g_srv.quality_level--;
                good_frames = 0;
                dispd_log("compositor: qualidade adaptativa recuperada -> nivel %d",
                          g_srv.quality_level);
            }
        }
        return;
    }

    /* INCREMENTAL: so os terminais que produziram saida (term->dirty) + a barra
     * (relogio/titulo) quando marcada. Cada regiao recompoe e apresenta so o seu
     * retangulo — o resto do backbuffer persiste do quadro anterior. */
    RECT dmg[258];
    int nd = 0;
    for (Window *w = g_srv.windows; w && nd < 256; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws &&
            w->kind == WK_TERM && w->term && w->term->dirty)
            dmg[nd++] = window_draw_rect(w);
    if (g_srv.internal_bar && g_srv.bar_dirty && g_srv.bar_h > 0) {
        RECT bar = { 0, 0, g_srv.scr_w, g_srv.bar_h };
        dmg[nd++] = bar;
    }
    if (nd == 0)
        return;
    for (int i = 0; i < nd; i++) {
        compose_region(&dmg[i]);
        present_frame(&dmg[i]);
    }
    g_srv.bar_dirty = 0;
}
