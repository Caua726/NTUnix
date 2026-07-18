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

    w->anim_ms = GetTickCount64();   /* #35: fade-in ao aparecer */
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

/* WK_FOREIGN: janela nativa do Windows que a gente gerencia (sem DIB nosso —
 * o win32k desenha; so cuidamos da geometria/foco/borda) */
Window *win_create_foreign(HWND hwnd)
{
    Window *w = (Window *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->id = ++g_srv.next_id;
    w->kind = WK_FOREIGN;
    w->ws = g_srv.cur_ws;
    w->visible = 1;
    w->border_px = 2;
    w->border_rgb = BORDER_NORMAL;
    w->hwnd = hwnd;
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

void win_focus(Window *w)
{
    /* audit #7: nao aceita foco em janela invisivel ou de outro workspace — o WM
     * pode mandar FOCUS de uma janela oculta ao remover a ultima do ws atual, e
     * o teclado iria pra uma janela que o usuario nao ve. Foco nulo nesse caso. */
    if (w && (!w->visible || w->ws != g_srv.cur_ws))
        w = NULL;
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
        draw_text_utf8(dc, g_srv.focused->title, &trc,
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
        int pct = 85;
        if (v[0]) { pct = 0; for (char *c = v; *c >= '0' && *c <= '9'; c++) pct = pct * 10 + (*c - '0'); }
        if (pct < 40) pct = 40;
        if (pct > 100) pct = 100;
        op = pct * 255 / 100;
    }
    return op;
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
    return r;
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
static int corner_radius(void)
{
    static int r = -1;
    if (r < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("DISPD_ROUND", v, sizeof v);
        if (v[0]) { r = atoi(v); if (r < 0) r = 0; if (r > 32) r = 32; }
        else r = 8;
    }
    return r;
}

/* ---- animacoes (aparecer): fade-in por tempo (#35 modernizacao) ---- */
#define ANIM_MS 160
static int anim_enabled(void)
{
    static int en = -1;
    if (en < 0) {
        char v[8] = "";
        GetEnvironmentVariableA("DISPD_ANIM", v, sizeof v);
        en = (v[0] == '0') ? 0 : 1;
    }
    return en;
}

/* alpha (0..255) da animacao de aparecer da janela; 255 = pronta. Zera anim_ms
 * ao terminar (ease-out cubico). */
static int win_anim_alpha(Window *w)
{
    if (!anim_enabled() || w->anim_ms == 0)
        return 255;
    ULONGLONG el = GetTickCount64() - w->anim_ms;
    if (el >= ANIM_MS) { w->anim_ms = 0; return 255; }
    int u = 255 - (int)(el * 255 / ANIM_MS);         /* 255 -> 0 */
    return 255 - (int)((long long)u * u * u / (255 * 255));  /* 1-(1-t)^3 */
}

int compositor_animating(void)
{
    if (!anim_enabled())
        return 0;
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->anim_ms != 0)
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
        if (w->visible && w->ws == g_srv.cur_ws)
            vis[nv++] = w;
    for (int i = 1; i < nv; i++) {
        Window *k = vis[i];
        int j = i - 1;
        while (j >= 0 && vis[j]->z > k->z) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = k;
    }
    return nv;
}

/* blenda o DIB da janela sobre o backbuffer (glass translucido), limitado a
 * `clip` (o retangulo de damage; NULL = sem limite). Opacidade cheia -> BitBlt
 * opaco (rapido). O src e' amostrado em (px-dx, py-dy). */
static void blit_glass(HDC memdc, const void *sbits, int sw,
                       int dx, int dy, int bw, int bh, const RECT *clip, int amul)
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

    int a = glass_opacity();
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

/* compoe UMA janela (borda + barra de abas + conteudo) dentro de `clip`. As
 * operacoes GDI ja saem clipadas pela regiao selecionada no cdc; o blit_glass
 * (loop de pixel manual) recebe o clip explicito. */
static void compose_one_window(Window *w, const RECT *clip)
{
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
                RECT tr = { tx + 4, iy, tx + twid - 4, iy + g_srv.title_h };
                draw_text_utf8(g_srv.cdc, lbl, &tr,
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
            draw_text_utf8(g_srv.cdc, base, &tr,
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
            blit_glass(w->memdc, w->bits, w->cw, ix, iy + g_srv.title_h, bw, bh, clip,
                       win_anim_alpha(w));   /* #35: fade-in ao aparecer */
        else                              /* app: superficie opaca (GDI clipa) */
            BitBlt(g_srv.cdc, ix, iy + g_srv.title_h, bw, bh, w->memdc, 0, 0, SRCCOPY);
    }

    /* cantos arredondados: recorta a moldura DEPOIS de tudo desenhado (garante
     * que o GDI da borda chegou ao DIB antes de sobrescrever os cantos) */
    if (corner_radius() > 0) {
        GdiFlush();
        round_corners(&w->rect, corner_radius(), clip);
    }
}

/* recompoe uma regiao do quadro: wallpaper + janelas que cruzam `r` (em ordem z)
 * + barra, tudo clipado a `r`. Base do damage tracking. */
static void compose_region(const RECT *r)
{
    draw_wallpaper_rect(r->left, r->top, r->right, r->bottom);

    HRGN rgn = CreateRectRgn(r->left, r->top, r->right, r->bottom);
    SelectClipRgn(g_srv.cdc, rgn);

    Window *vis[256];
    int nv = collect_visible(vis, 256);
    for (int i = 0; i < nv; i++)
        if (rects_hit(&vis[i]->rect, r))
            compose_one_window(vis[i], r);

    RECT bar = { 0, 0, g_srv.scr_w, g_srv.bar_h };
    if (g_srv.bar_h > 0 && rects_hit(&bar, r))
        draw_bar();

    SelectClipRgn(g_srv.cdc, NULL);
    DeleteObject(rgn);
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
    /* propaga titulos de todas as visiveis (eventos independem do damage) */
    for (Window *w = g_srv.windows; w; w = w->next)
        if (w->visible && w->ws == g_srv.cur_ws)
            pump_title(w);

    /* recompose TOTAL: qualquer mudanca estrutural (layout/foco/criar/destruir/
     * app-commit) seta g_srv.dirty -> repinta a tela inteira (fallback seguro). */
    if (g_srv.dirty || !damage_enabled()) {
        RECT full = { 0, 0, g_srv.scr_w, g_srv.scr_h };
        compose_region(&full);
        present_frame(NULL);
        g_srv.dirty = 0;
        g_srv.bar_dirty = 0;
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
            dmg[nd++] = w->rect;
    if (g_srv.bar_dirty && g_srv.bar_h > 0) {
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
