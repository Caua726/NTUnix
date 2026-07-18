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

    /* fonte monoespacada garantida no WinPE; metricas com defaults se algo
     * falhar (#96: nao le TEXTMETRIC nao inicializado) */
    g_srv.font = (HFONT)GetStockObject(OEM_FIXED_FONT);
    g_srv.cellw = 8;
    g_srv.cellh = 16;
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
    w->ws = g_srv.cur_ws;
    w->visible = 1;
    w->border_px = 2;
    w->border_rgb = BORDER_NORMAL;
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
    w->ws = g_srv.cur_ws;
    w->visible = 1;
    w->border_px = 2;
    w->border_rgb = BORDER_NORMAL;
    w->cw = cw;
    w->ch = ch;
    w->section = section;

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

void win_focus(Window *w)
{
    for (Window *o = g_srv.windows; o; o = o->next)
        o->focused = 0;
    if (w) {
        w->focused = 1;
        g_srv.focused = w;
    } else {
        g_srv.focused = NULL;   /* #7: limpa o ponteiro tambem */
    }
    g_srv.dirty = 1;
}

Window *win_at_point(int x, int y)
{
    Window *hit = NULL;
    for (Window *w = g_srv.windows; w; w = w->next) {
        if (!w->visible || w->ws != g_srv.cur_ws)
            continue;
        if (x >= w->rect.left && x < w->rect.right &&
            y >= w->rect.top && y < w->rect.bottom) {
            if (!hit || w->z >= hit->z)
                hit = w;
        }
    }
    return hit;
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

/* barra de status no topo: workspaces + titulo focado + relogio.
 * Desenhada pelo dispd (dono da fonte/GDI); estilo dwm drawbar. */
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
        DrawTextA(dc, g_srv.focused->title, -1, &trc,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    SetTextColor(dc, RGB(150, 150, 170));
    TextOutA(dc, clock_x, ty, ts, (int)strlen(ts));

    SelectObject(dc, of);
}

/* ---- glass: wallpaper com gradiente + composicao translucida ---- */

/* opacidade do conteudo (0..255); DISPD_OPACITY em % (default 92) */
static int glass_opacity(void)
{
    static int op = -1;
    if (op < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("DISPD_OPACITY", v, sizeof v);
        int pct = 92;
        if (v[0]) { pct = 0; for (char *c = v; *c >= '0' && *c <= '9'; c++) pct = pct * 10 + (*c - '0'); }
        if (pct < 40) pct = 40;
        if (pct > 100) pct = 100;
        op = pct * 255 / 100;
    }
    return op;
}

/* gradiente vertical no backbuffer (escuro-azulado -> roxo-escuro) */
static void draw_wallpaper(void)
{
    unsigned char *p = (unsigned char *)g_srv.cbits;
    int W = g_srv.scr_w, H = g_srv.scr_h, stride = g_srv.frame.stride;
    if (!p)
        return;
    for (int y = 0; y < H; y++) {
        int tt = H > 1 ? (y * 255 / (H - 1)) : 0;
        unsigned char r = (unsigned char)(20 + (40 - 20) * tt / 255);
        unsigned char g = (unsigned char)(20 + (26 - 20) * tt / 255);
        unsigned char b = (unsigned char)(30 + (56 - 30) * tt / 255);
        unsigned char *row = p + (size_t)y * stride;
        for (int x = 0; x < W; x++) {
            row[x * 4 + 0] = b; row[x * 4 + 1] = g; row[x * 4 + 2] = r; row[x * 4 + 3] = 255;
        }
    }
}

/* blenda o DIB da janela sobre o backbuffer (glass translucido); com opacidade
 * cheia cai no BitBlt opaco (rapido) */
static void blit_glass(HDC memdc, const void *sbits, int sw,
                       int dx, int dy, int bw, int bh)
{
    if (glass_opacity() >= 255) {
        BitBlt(g_srv.cdc, dx, dy, bw, bh, memdc, 0, 0, SRCCOPY);
        return;
    }
    GdiFlush();   /* garante que o ExtTextOut do vt_render chegou ao DIB */
    unsigned char *dst = (unsigned char *)g_srv.cbits;
    const unsigned char *src = (const unsigned char *)sbits;
    if (!dst || !src) return;
    int W = g_srv.scr_w, H = g_srv.scr_h, dstride = g_srv.frame.stride, sstride = sw * 4;
    int a = glass_opacity(), ia = 255 - a;
    for (int y = 0; y < bh; y++) {
        int py = dy + y;
        if (py < 0 || py >= H) continue;
        unsigned char *drow = dst + (size_t)py * dstride;
        const unsigned char *srow = src + (size_t)y * sstride;
        for (int x = 0; x < bw; x++) {
            int px = dx + x;
            if (px < 0 || px >= W) continue;
            unsigned char *d = drow + px * 4;
            const unsigned char *s = srow + x * 4;
            d[0] = (unsigned char)((s[0] * a + d[0] * ia) >> 8);
            d[1] = (unsigned char)((s[1] * a + d[1] * ia) >> 8);
            d[2] = (unsigned char)((s[2] * a + d[2] * ia) >> 8);
        }
    }
}

void compose_and_present(void)
{
    /* fundo: gradiente (o glass do terminal deixa ele aparecer) */
    draw_wallpaper();

    /* coleta visiveis do ws atual */
    Window *vis[256];
    int nv = 0;
    for (Window *w = g_srv.windows; w && nv < 256; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws)
            vis[nv++] = w;
    /* ordena por z crescente (insertion sort, N pequeno) */
    for (int i = 1; i < nv; i++) {
        Window *k = vis[i];
        int j = i - 1;
        while (j >= 0 && vis[j]->z > k->z) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = k;
    }

    for (int i = 0; i < nv; i++) {
        Window *w = vis[i];
        pump_title(w);
        if (w->kind == WK_TERM && w->term && w->term->dirty)
            vt_render(w->term, w->memdc, g_srv.font, g_srv.cellw, g_srv.cellh);

        /* borda: MOLDURA (nao preenche o miolo -> o wallpaper aparece sob o
         * glass do terminal). foco = destaque */
        COLORREF bc = w->focused ? BORDER_FOCUS : w->border_rgb;
        HBRUSH bb = CreateSolidBrush(bc);
        int bp = w->border_px;
        RECT edges[4] = {
            { w->rect.left, w->rect.top, w->rect.right, w->rect.top + bp },
            { w->rect.left, w->rect.bottom - bp, w->rect.right, w->rect.bottom },
            { w->rect.left, w->rect.top, w->rect.left + bp, w->rect.bottom },
            { w->rect.right - bp, w->rect.top, w->rect.right, w->rect.bottom },
        };
        for (int e = 0; e < 4; e++)
            FillRect(g_srv.cdc, &edges[e], bb);
        DeleteObject(bb);

        int ix = w->rect.left + w->border_px;
        int iy = w->rect.top + w->border_px;
        int iw = (w->rect.right - w->rect.left) - 2 * w->border_px;

        /* barra de ABAS do terminal (i3-ish); barra de titulo simples p/ apps */
        if (g_srv.title_h > 0 && iw > 0) {
            SelectObject(g_srv.cdc, g_srv.font);
            SetBkMode(g_srv.cdc, TRANSPARENT);
            if (w->kind == WK_TERM && w->ntabs > 0) {
                int tw = iw / w->ntabs;
                if (tw < 1) tw = 1;
                for (int ti = 0; ti < w->ntabs; ti++) {
                    int tx = ix + ti * tw;
                    int twid = (ti == w->ntabs - 1) ? (ix + iw - tx) : tw;
                    int act = (ti == w->active_tab);
                    RECT tb = { tx, iy, tx + twid, iy + g_srv.title_h };
                    COLORREF tc = act ? (w->focused ? BORDER_FOCUS : RGB(70, 70, 84))
                                      : RGB(30, 30, 38);
                    HBRUSH tbb = CreateSolidBrush(tc);
                    FillRect(g_srv.cdc, &tb, tbb);
                    DeleteObject(tbb);
                    Terminal *tt = w->tabs[ti];
                    const char *base = (tt && tt->title[0]) ? tt->title : "sh";
                    char lbl[288];
                    snprintf(lbl, sizeof lbl, " %d %s", ti + 1, base);
                    SetTextColor(g_srv.cdc, act
                        ? (w->focused ? RGB(12, 14, 20) : RGB(225, 225, 235))
                        : RGB(150, 150, 165));
                    RECT tr = { tx + 4, iy, tx + twid - 4, iy + g_srv.title_h };
                    DrawTextA(g_srv.cdc, lbl, -1, &tr,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            } else {
                RECT tb = { ix, iy, ix + iw, iy + g_srv.title_h };
                COLORREF tc = w->focused ? BORDER_FOCUS : RGB(44, 44, 52);
                HBRUSH tbb = CreateSolidBrush(tc);
                FillRect(g_srv.cdc, &tb, tbb);
                DeleteObject(tbb);
                SetTextColor(g_srv.cdc, w->focused ? RGB(12, 14, 20)
                                                   : RGB(180, 180, 195));
                const char *base = w->title[0] ? w->title : "app";
                RECT tr = { ix + 6, iy, ix + iw - 6, iy + g_srv.title_h };
                DrawTextA(g_srv.cdc, base, -1, &tr,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
        }

        /* conteudo (terminal/app) abaixo da barra de titulo, SEMPRE clipado a
         * area cliente: uma surface de app maior que o tile nao pode invadir a
         * janela vizinha nem sobrar fundo de borda (#49) */
        int ih = (w->rect.bottom - w->rect.top) - 2 * w->border_px - g_srv.title_h;
        int bw = w->cw < iw ? w->cw : iw;
        int bh = w->ch < ih ? w->ch : ih;
        if (bw > 0 && bh > 0) {
            if (w->kind == WK_TERM)          /* glass: terminal translucido */
                blit_glass(w->memdc, w->bits, w->cw, ix, iy + g_srv.title_h, bw, bh);
            else                              /* app: superficie opaca */
                BitBlt(g_srv.cdc, ix, iy + g_srv.title_h, bw, bh, w->memdc, 0, 0, SRCCOPY);
        }
    }

    draw_bar();

    if (g_srv.present)
        g_srv.present->present(g_srv.present, &g_srv.frame);
}
