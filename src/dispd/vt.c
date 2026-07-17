/*
 * vt.c — parser VT/ANSI minimo (output-only) + render na grade de celulas.
 *
 * Modelado no st (suckless terminal): dispatch de tputc por byte, csihandle
 * para CSI (CUU/CUD/CUF/CUB/CUP, ED/EL, SGR), tnewline/tscrollup para rolagem.
 * Single-byte (ASCII/CP437) — suficiente para o M1/M2 com OEM_FIXED_FONT.
 *
 * Referencias reais: st.c (git.suckless.org/st) — tputc/csihandle/tsetattr.
 */
#include "term.h"

/* paleta ANSI 16 cores (aprox. xterm) — indices 0..15 */
static const COLORREF g_pal[16] = {
    RGB(  0,   0,   0), RGB(205,   0,   0), RGB(  0, 205,   0), RGB(205, 205,   0),
    RGB(  0,   0, 238), RGB(205,   0, 205), RGB(  0, 205, 205), RGB(229, 229, 229),
    RGB(127, 127, 127), RGB(255,   0,   0), RGB(  0, 255,   0), RGB(255, 255,   0),
    RGB( 92,  92, 255), RGB(255,   0, 255), RGB(  0, 255, 255), RGB(255, 255, 255),
};

#define DEF_FG 7
#define DEF_BG 0

static Cell *cell_at(Terminal *t, int x, int y)
{
    return &t->grid[y * t->cols + x];
}

static void clear_cell(Terminal *t, Cell *c)
{
    c->ch = ' ';
    c->fg = t->cur_fg;
    c->bg = t->cur_bg;
    c->attr = 0;
}

static void clear_region(Terminal *t, int x0, int y0, int x1, int y1)
{
    for (int y = y0; y <= y1 && y < t->rows; y++)
        for (int x = x0; x <= x1 && x < t->cols; x++)
            clear_cell(t, cell_at(t, x, y));
}

void vt_init(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;
    t->grid = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    t->cur_x = t->cur_y = 0;
    t->cur_vis = 1;
    t->vt_state = VT_GROUND;
    t->cur_fg = DEF_FG;
    t->cur_bg = DEF_BG;
    t->cur_attr = 0;
    if (t->grid)
        clear_region(t, 0, 0, cols - 1, rows - 1);
}

void vt_free(Terminal *t)
{
    free(t->grid);
    t->grid = NULL;
}

void vt_resize(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows)
        return;
    Cell *ng = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    if (!ng)
        return;
    /* preserva o canto superior-esquerdo */
    int cx = t->cols < cols ? t->cols : cols;
    int cy = t->rows < rows ? t->rows : rows;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cx; x++)
            ng[y * cols + x] = t->grid[y * t->cols + x];
    free(t->grid);
    t->grid = ng;
    t->cols = cols;
    t->rows = rows;
    if (t->cur_x >= cols) t->cur_x = cols - 1;
    if (t->cur_y >= rows) t->cur_y = rows - 1;
}

static void scroll_up(Terminal *t)
{
    /* sobe uma linha; limpa a ultima */
    memmove(t->grid, t->grid + t->cols,
            (size_t)(t->rows - 1) * t->cols * sizeof(Cell));
    for (int x = 0; x < t->cols; x++)
        clear_cell(t, cell_at(t, x, t->rows - 1));
}

static void newline(Terminal *t)
{
    if (t->cur_y >= t->rows - 1)
        scroll_up(t);
    else
        t->cur_y++;
}

static void put_char(Terminal *t, unsigned char ch)
{
    if (t->cur_x >= t->cols) {   /* wrap */
        t->cur_x = 0;
        newline(t);
    }
    Cell *c = cell_at(t, t->cur_x, t->cur_y);
    c->ch = ch;
    c->fg = t->cur_fg;
    c->bg = t->cur_bg;
    c->attr = t->cur_attr;
    t->cur_x++;
}

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* SGR: aplica os parametros de 'm' (st tsetattr, mapeamento 30-37/40-47/90-97) */
static void sgr(Terminal *t)
{
    int n = t->nparams ? t->nparams : 1;
    for (int i = 0; i < n; i++) {
        int a = t->params[i];
        if (a == 0) { t->cur_fg = DEF_FG; t->cur_bg = DEF_BG; t->cur_attr = 0; }
        else if (a == 1) t->cur_attr |= ATTR_BOLD;
        else if (a == 7) t->cur_attr |= ATTR_REVERSE;
        else if (a == 22) t->cur_attr &= ~ATTR_BOLD;
        else if (a == 27) t->cur_attr &= ~ATTR_REVERSE;
        else if (a >= 30 && a <= 37) t->cur_fg = (unsigned char)(a - 30);
        else if (a == 39) t->cur_fg = DEF_FG;
        else if (a >= 40 && a <= 47) t->cur_bg = (unsigned char)(a - 40);
        else if (a == 49) t->cur_bg = DEF_BG;
        else if (a >= 90 && a <= 97) t->cur_fg = (unsigned char)(a - 90 + 8);
        else if (a >= 100 && a <= 107) t->cur_bg = (unsigned char)(a - 100 + 8);
        else if (a == 38 || a == 48) {
            /* 38;5;n (indexado) ou 38;2;r;g;b (truecolor) — mapeia so 0..15 */
            if (i + 2 < n && t->params[i + 1] == 5) {
                int idx = t->params[i + 2];
                if (idx >= 0 && idx <= 15) {
                    if (a == 38) t->cur_fg = (unsigned char)idx;
                    else t->cur_bg = (unsigned char)idx;
                }
                i += 2;
            } else if (i + 4 < n && t->params[i + 1] == 2) {
                i += 4;  /* truecolor: ignora, mantem cor atual */
            }
        }
    }
}

static void csi_dispatch(Terminal *t, unsigned char final)
{
    int a0 = t->nparams > 0 ? t->params[0] : 0;
    switch (final) {
    case 'A': t->cur_y = clampi(t->cur_y - (a0 ? a0 : 1), 0, t->rows - 1); break;
    case 'B': t->cur_y = clampi(t->cur_y + (a0 ? a0 : 1), 0, t->rows - 1); break;
    case 'C': t->cur_x = clampi(t->cur_x + (a0 ? a0 : 1), 0, t->cols - 1); break;
    case 'D': t->cur_x = clampi(t->cur_x - (a0 ? a0 : 1), 0, t->cols - 1); break;
    case 'G': t->cur_x = clampi((a0 ? a0 : 1) - 1, 0, t->cols - 1); break;
    case 'd': t->cur_y = clampi((a0 ? a0 : 1) - 1, 0, t->rows - 1); break;
    case 'H':
    case 'f': {
        int row = t->nparams > 0 && t->params[0] ? t->params[0] : 1;
        int col = t->nparams > 1 && t->params[1] ? t->params[1] : 1;
        t->cur_y = clampi(row - 1, 0, t->rows - 1);
        t->cur_x = clampi(col - 1, 0, t->cols - 1);
        break;
    }
    case 'J':   /* ED */
        if (a0 == 2)      clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
        else if (a0 == 1) clear_region(t, 0, 0, t->cols - 1, t->cur_y);
        else              clear_region(t, t->cur_x, t->cur_y, t->cols - 1, t->rows - 1);
        break;
    case 'K':   /* EL */
        if (a0 == 2)      clear_region(t, 0, t->cur_y, t->cols - 1, t->cur_y);
        else if (a0 == 1) clear_region(t, 0, t->cur_y, t->cur_x, t->cur_y);
        else              clear_region(t, t->cur_x, t->cur_y, t->cols - 1, t->cur_y);
        break;
    case 'm': sgr(t); break;
    case 'h':
    case 'l':
        if (t->priv && a0 == 25) t->cur_vis = (final == 'h');  /* DECTCEM */
        break;
    default: break;   /* nao suportado: ignora */
    }
}

/* dispatch por byte (st tputc). Chamador ja segurou o lock. */
static void feed_byte(Terminal *t, unsigned char u)
{
    switch (t->vt_state) {
    case VT_GROUND:
        switch (u) {
        case '\r': t->cur_x = 0; break;
        case '\n': case '\v': case '\f': newline(t); break;
        case '\b': if (t->cur_x > 0) t->cur_x--; break;
        case '\t': t->cur_x = clampi((t->cur_x & ~7) + 8, 0, t->cols - 1); break;
        case '\a': break;
        case 0x1b: t->vt_state = VT_ESC; break;
        default:
            if (u >= 0x20) put_char(t, u);
            break;
        }
        break;
    case VT_ESC:
        if (u == '[') {
            t->vt_state = VT_CSI;
            t->nparams = 0;
            t->priv = 0;
            memset(t->params, 0, sizeof t->params);
        } else if (u == ']') {
            t->vt_state = VT_OSC;
            t->osc_len = 0;
        } else if (u == 'c') {   /* RIS: reset */
            t->cur_x = t->cur_y = 0;
            t->cur_fg = DEF_FG; t->cur_bg = DEF_BG; t->cur_attr = 0;
            clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
            t->vt_state = VT_GROUND;
        } else {
            t->vt_state = VT_GROUND;   /* ESC de 1 byte nao tratado */
        }
        break;
    case VT_CSI:
        if (u == '?') {
            t->priv = 1;
        } else if (u >= '0' && u <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (t->nparams <= 8)
                *p = *p * 10 + (u - '0');
        } else if (u == ';') {
            if (t->nparams < 8) t->nparams++;
            else t->nparams = 8;
        } else if (u >= 0x40 && u <= 0x7e) {
            csi_dispatch(t, u);
            t->vt_state = VT_GROUND;
        } else {
            /* intermediarios (espaco, etc): ignora */
        }
        break;
    case VT_OSC:
        if (u == 0x07 || u == 0x1b) {   /* BEL ou ST termina o OSC */
            /* OSC 0;titulo ou 2;titulo -> titulo da janela */
            t->osc[t->osc_len] = 0;
            if ((t->osc_len > 2) &&
                (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';') {
                strncpy(t->title, t->osc + 2, sizeof t->title - 1);
                t->title[sizeof t->title - 1] = 0;
                t->title_changed = 1;   /* main thread propaga p/ a janela */
            }
            t->vt_state = (u == 0x1b) ? VT_ESC : VT_GROUND;
        } else if (t->osc_len < (int)sizeof t->osc - 1) {
            t->osc[t->osc_len++] = (char)u;
        }
        break;
    }
}

void vt_feed(Terminal *t, const char *bytes, int n)
{
    EnterCriticalSection(&t->lock);
    for (int i = 0; i < n; i++)
        feed_byte(t, (unsigned char)bytes[i]);
    t->dirty = 1;
    t->rx += n;
    LeaveCriticalSection(&t->lock);
}

/* ---- render da grade no DIB (main thread) ---- */

static Cell *g_snap;
static size_t g_snap_cap;

void vt_render(Terminal *t, HDC memdc, HFONT font, int cellw, int cellh)
{
    int cols, rows, cx, cy, cvis;

    EnterCriticalSection(&t->lock);
    cols = t->cols;
    rows = t->rows;
    cx = t->cur_x; cy = t->cur_y; cvis = t->cur_vis;
    size_t need = (size_t)cols * rows * sizeof(Cell);
    if (need > g_snap_cap) {
        Cell *ns = (Cell *)realloc(g_snap, need);
        if (ns) { g_snap = ns; g_snap_cap = need; }
    }
    if (g_snap && need <= g_snap_cap && t->grid)
        memcpy(g_snap, t->grid, need);
    t->dirty = 0;
    LeaveCriticalSection(&t->lock);

    if (!g_snap || need > g_snap_cap)
        return;

    HFONT old = (HFONT)SelectObject(memdc, font);
    SetBkMode(memdc, OPAQUE);

    for (int y = 0; y < rows; y++) {
        int x = 0;
        while (x < cols) {
            Cell *c0 = &g_snap[y * cols + x];
            /* junta um run com mesmo fg/bg/attr */
            int run = 1;
            while (x + run < cols) {
                Cell *cn = &g_snap[y * cols + x + run];
                if (cn->fg != c0->fg || cn->bg != c0->bg || cn->attr != c0->attr)
                    break;
                run++;
            }
            int fg = c0->fg & 15, bg = c0->bg & 15;
            if (c0->attr & ATTR_REVERSE) { int tmp = fg; fg = bg; bg = tmp; }
            if ((c0->attr & ATTR_BOLD) && fg < 8) fg += 8;
            SetTextColor(memdc, g_pal[fg]);
            SetBkColor(memdc, g_pal[bg]);
            char buf[512];
            int bn = run < (int)sizeof buf ? run : (int)sizeof buf - 1;
            for (int i = 0; i < bn; i++) {
                unsigned char ch = g_snap[y * cols + x + i].ch;
                buf[i] = (ch < 0x20) ? ' ' : (char)ch;
            }
            RECT rc = { x * cellw, y * cellh, (x + bn) * cellw, (y + 1) * cellh };
            ExtTextOutA(memdc, x * cellw, y * cellh, ETO_OPAQUE | ETO_CLIPPED,
                        &rc, buf, bn, NULL);
            x += run;
        }
    }

    /* cursor: bloco invertido */
    if (cvis && cx < cols && cy < rows) {
        RECT rc = { cx * cellw, cy * cellh, (cx + 1) * cellw, (cy + 1) * cellh };
        InvertRect(memdc, &rc);
    }

    SelectObject(memdc, old);
}
