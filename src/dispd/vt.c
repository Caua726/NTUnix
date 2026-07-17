/*
 * vt.c — emulador VT/ANSI + render na grade de celulas.
 *
 * Maquina de estados completa modelada no st/xterm: GROUND/ESC/CSI/OSC/STR,
 * tela primaria + alternada, regiao de rolagem (DECSTBM), autowrap com wrap
 * adiado (DECAWM), origin mode (DECOM), save/restore de cursor (DECSC/DECRC),
 * IL/DL/ICH/DCH/ECH/SU/SD, SGR com 16/256/truecolor, e — crucial — RESPONDE a
 * DSR/CPR/DA escrevendo de volta no pty (sem isso o busybox lineedit trava
 * esperando a posicao do cursor no ESC[6n antes de imprimir o prompt).
 *
 * Decodifica UTF-8 em codepoints (1 celula por codepoint); o render usa a
 * fonte OEM (CP437) — ASCII direto, um punhado de box-drawing mapeado, resto
 * '?'. As cores sao COLORREF resolvidas, entao truecolor sai exato.
 *
 * Referencias reais: st.c (git.suckless.org/st) — tputc/csihandle/tsetattr/
 * tscrollup; VT100/VT220 (DEC STD 070) para as sequencias.
 */
#include "term.h"

#define DEF_FG  RGB(0xE5, 0xE5, 0xE5)
#define DEF_BG  RGB(0x00, 0x00, 0x00)

/* paleta ANSI base 0..15 (aprox. xterm) */
static const COLORREF g_pal16[16] = {
    RGB(  0,   0,   0), RGB(205,   0,   0), RGB(  0, 205,   0), RGB(205, 205,   0),
    RGB(  0,   0, 238), RGB(205,   0, 205), RGB(  0, 205, 205), RGB(229, 229, 229),
    RGB(127, 127, 127), RGB(255,   0,   0), RGB(  0, 255,   0), RGB(255, 255,   0),
    RGB( 92,  92, 255), RGB(255,   0, 255), RGB(  0, 255, 255), RGB(255, 255, 255),
};

COLORREF vt_ansi_color(int idx)
{
    if (idx < 0) idx = 0;
    return g_pal16[idx & 15];
}

/* xterm 256: 0..15 base, 16..231 cubo 6x6x6, 232..255 rampa de cinza */
static COLORREF pal256(int i)
{
    if (i < 16)
        return g_pal16[i];
    if (i < 232) {
        int n = i - 16;
        int r = (n / 36) % 6, g = (n / 6) % 6, b = n % 6;
        int cv[6] = { 0, 95, 135, 175, 215, 255 };
        return RGB(cv[r], cv[g], cv[b]);
    }
    int v = 8 + (i - 232) * 10;
    return RGB(v, v, v);
}

/* ---- grade ---- */

static Cell *cell_at(Terminal *t, int x, int y)
{
    return &t->grid[y * t->cols + x];
}

static void set_blank(Terminal *t, Cell *c)
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
            if (x >= 0 && y >= 0)
                set_blank(t, cell_at(t, x, y));
}

static void grid_reset_defaults(Terminal *t)
{
    t->cur_fg = DEF_FG;
    t->cur_bg = DEF_BG;
    t->cur_attr = 0;
}

int vt_init(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;
    t->grid_main = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    t->grid_alt  = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    t->tabs      = (char *)calloc((size_t)cols, 1);
    if (!t->grid_main || !t->grid_alt || !t->tabs) {   /* #105 */
        free(t->grid_main); free(t->grid_alt); free(t->tabs);
        t->grid_main = t->grid_alt = NULL; t->tabs = NULL;
        return -1;
    }
    t->grid = t->grid_main;
    t->on_alt = 0;
    t->cur_x = t->cur_y = 0;
    t->wrap_pending = 0;
    t->cur_vis = 1;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    t->mode_autowrap = 1;
    t->mode_origin = 0;
    t->vt_state = VT_GROUND;
    grid_reset_defaults(t);
    for (int x = 8; x < cols; x += 8)
        t->tabs[x] = 1;
    clear_region(t, 0, 0, cols - 1, rows - 1);
    /* limpa tambem a tela alternada */
    Cell *save = t->grid; t->grid = t->grid_alt;
    clear_region(t, 0, 0, cols - 1, rows - 1);
    t->grid = save;
    return 0;
}

void vt_free(Terminal *t)
{
    free(t->grid_main);
    free(t->grid_alt);
    free(t->tabs);
    t->grid = t->grid_main = t->grid_alt = NULL;
    t->tabs = NULL;
}

/* preserva o canto superior-esquerdo de um buffer ao redimensionar */
static void regrid(Terminal *t, Cell **buf, int cols, int rows)
{
    Cell *ng = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    if (!ng)
        return;
    for (int i = 0; i < cols * rows; i++) { ng[i].ch = ' '; ng[i].fg = DEF_FG; ng[i].bg = DEF_BG; }
    int cx = t->cols < cols ? t->cols : cols;
    int cy = t->rows < rows ? t->rows : rows;
    for (int y = 0; y < cy; y++)
        for (int x = 0; x < cx; x++)
            ng[y * cols + x] = (*buf)[y * t->cols + x];
    free(*buf);
    *buf = ng;
}

void vt_resize(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows)
        return;
    int active_alt = t->on_alt;
    regrid(t, &t->grid_main, cols, rows);
    regrid(t, &t->grid_alt, cols, rows);
    char *nt = (char *)calloc((size_t)cols, 1);
    if (nt) {
        for (int x = 8; x < cols; x += 8) nt[x] = 1;
        free(t->tabs);
        t->tabs = nt;
    }
    t->cols = cols;
    t->rows = rows;
    t->grid = active_alt ? t->grid_alt : t->grid_main;
    t->scroll_top = 0;
    t->scroll_bot = rows - 1;
    if (t->cur_x >= cols) t->cur_x = cols - 1;
    if (t->cur_y >= rows) t->cur_y = rows - 1;
    t->wrap_pending = 0;
}

/* ---- rolagem dentro da regiao ---- */

static void scroll_up(Terminal *t, int n)
{
    int top = t->scroll_top, bot = t->scroll_bot;
    if (n < 1) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int y = top; y <= bot - n; y++)
        memcpy(&t->grid[y * t->cols], &t->grid[(y + n) * t->cols],
               (size_t)t->cols * sizeof(Cell));
    for (int y = bot - n + 1; y <= bot; y++)
        for (int x = 0; x < t->cols; x++)
            set_blank(t, cell_at(t, x, y));
}

static void scroll_down(Terminal *t, int n)
{
    int top = t->scroll_top, bot = t->scroll_bot;
    if (n < 1) return;
    if (n > bot - top + 1) n = bot - top + 1;
    for (int y = bot; y >= top + n; y--)
        memcpy(&t->grid[y * t->cols], &t->grid[(y - n) * t->cols],
               (size_t)t->cols * sizeof(Cell));
    for (int y = top; y < top + n; y++)
        for (int x = 0; x < t->cols; x++)
            set_blank(t, cell_at(t, x, y));
}

/* LF/IND: desce respeitando a regiao */
static void do_index(Terminal *t)
{
    if (t->cur_y == t->scroll_bot)
        scroll_up(t, 1);
    else if (t->cur_y < t->rows - 1)
        t->cur_y++;
}

static void do_rindex(Terminal *t)   /* RI: sobe */
{
    if (t->cur_y == t->scroll_top)
        scroll_down(t, 1);
    else if (t->cur_y > 0)
        t->cur_y--;
}

/* ---- largura de codepoint (wcwidth minimo) ---- */

static int cp_width(unsigned cp)
{
    if (cp == 0)
        return 0;
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
        (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
        (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD))
        return 2;
    return 1;
}

static void put_cp(Terminal *t, unsigned cp)
{
    int w = cp_width(cp);
    if (w == 0)
        return;   /* combining: ignora (sem suporte a composicao) */
    if (t->wrap_pending) {
        t->cur_x = 0;
        do_index(t);
        t->wrap_pending = 0;
    }
    if (t->cur_x + w > t->cols) {   /* nao cabe: quebra agora */
        t->cur_x = 0;
        do_index(t);
    }
    Cell *c = cell_at(t, t->cur_x, t->cur_y);
    c->ch = cp;
    c->fg = t->cur_fg;
    c->bg = t->cur_bg;
    c->attr = t->cur_attr;
    if (w == 2 && t->cur_x + 1 < t->cols) {
        Cell *c2 = cell_at(t, t->cur_x + 1, t->cur_y);
        c2->ch = ' ';
        c2->fg = t->cur_fg;
        c2->bg = t->cur_bg;
        c2->attr = (unsigned short)(t->cur_attr | ATTR_WDUMMY);
    }
    t->cur_x += w;
    if (t->cur_x >= t->cols) {
        t->cur_x = t->cols - 1;
        if (t->mode_autowrap)
            t->wrap_pending = 1;
    }
}

/* ---- resposta a queries (montada sob lock, enviada depois) ---- */

static void reply(Terminal *t, const char *s)
{
    int n = (int)strlen(s);
    if (t->reply_len + n > VT_REPLYCAP)
        return;
    memcpy(t->reply + t->reply_len, s, n);
    t->reply_len += n;
}

/* ---- SGR ---- */

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void sgr(Terminal *t)
{
    int n = t->nparams ? t->nparams : 1;
    for (int i = 0; i < n; i++) {
        int a = t->params[i];
        if (a == 0) { grid_reset_defaults(t); }
        else if (a == 1)  t->cur_attr |= ATTR_BOLD;
        else if (a == 2)  t->cur_attr |= ATTR_DIM;
        else if (a == 3)  t->cur_attr |= ATTR_ITALIC;
        else if (a == 4)  t->cur_attr |= ATTR_UNDERLINE;
        else if (a == 5 || a == 6) t->cur_attr |= ATTR_BLINK;
        else if (a == 7)  t->cur_attr |= ATTR_REVERSE;
        else if (a == 8)  t->cur_attr |= ATTR_INVIS;
        else if (a == 22) t->cur_attr &= ~(ATTR_BOLD | ATTR_DIM);
        else if (a == 23) t->cur_attr &= ~ATTR_ITALIC;
        else if (a == 24) t->cur_attr &= ~ATTR_UNDERLINE;
        else if (a == 25) t->cur_attr &= ~ATTR_BLINK;
        else if (a == 27) t->cur_attr &= ~ATTR_REVERSE;
        else if (a == 28) t->cur_attr &= ~ATTR_INVIS;
        else if (a >= 30 && a <= 37) t->cur_fg = g_pal16[a - 30];
        else if (a == 39) t->cur_fg = DEF_FG;
        else if (a >= 40 && a <= 47) t->cur_bg = g_pal16[a - 40];
        else if (a == 49) t->cur_bg = DEF_BG;
        else if (a >= 90 && a <= 97)   t->cur_fg = g_pal16[a - 90 + 8];
        else if (a >= 100 && a <= 107) t->cur_bg = g_pal16[a - 100 + 8];
        else if (a == 38 || a == 48) {
            COLORREF col; int consumed = 0;
            if (i + 2 < n && t->params[i + 1] == 5) {
                col = pal256(clampi(t->params[i + 2], 0, 255));
                consumed = 2;
            } else if (i + 4 < n && t->params[i + 1] == 2) {
                col = RGB(clampi(t->params[i + 2], 0, 255),
                          clampi(t->params[i + 3], 0, 255),
                          clampi(t->params[i + 4], 0, 255));
                consumed = 4;
            } else break;
            if (a == 38) t->cur_fg = col; else t->cur_bg = col;
            i += consumed;
        }
    }
}

/* ---- modos DEC privados (CSI ? Pm h/l) ---- */

static void switch_screen(Terminal *t, int alt)
{
    if (alt == t->on_alt)
        return;
    t->on_alt = alt;
    t->grid = alt ? t->grid_alt : t->grid_main;
}

static void set_mode(Terminal *t, int set)
{
    for (int i = 0; i < (t->nparams ? t->nparams : 1); i++) {
        int a = t->params[i];
        if (t->priv == '?') {
            switch (a) {
            case 1:    t->mode_appcursor = set; break;      /* DECCKM */
            case 6:    t->mode_origin = set;                /* DECOM */
                       t->cur_x = 0;
                       t->cur_y = set ? t->scroll_top : 0;
                       t->wrap_pending = 0; break;
            case 7:    t->mode_autowrap = set; break;       /* DECAWM */
            case 25:   t->cur_vis = set; break;             /* DECTCEM */
            case 1000: if (set) t->mode_mouse |= 1; else t->mode_mouse &= ~1; break;
            case 1002: if (set) t->mode_mouse |= 2; else t->mode_mouse &= ~2; break;
            case 1003: if (set) t->mode_mouse |= 4; else t->mode_mouse &= ~4; break;
            case 1006: t->mode_mouse_sgr = set; break;
            case 2004: t->mode_bracketed = set; break;
            case 47:
            case 1047: switch_screen(t, set);
                       if (set) clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
                       break;
            case 1048: if (set) { t->sv_x = t->cur_x; t->sv_y = t->cur_y;
                                  t->sv_fg = t->cur_fg; t->sv_bg = t->cur_bg;
                                  t->sv_attr = t->cur_attr; }
                       else { t->cur_x = t->sv_x; t->cur_y = t->sv_y;
                              t->cur_fg = t->sv_fg; t->cur_bg = t->sv_bg;
                              t->cur_attr = t->sv_attr; } break;
            case 1049:
                if (set) {
                    t->sv_x = t->cur_x; t->sv_y = t->cur_y;
                    t->sv_fg = t->cur_fg; t->sv_bg = t->cur_bg; t->sv_attr = t->cur_attr;
                    switch_screen(t, 1);
                    clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
                    t->cur_x = t->cur_y = 0;
                } else {
                    switch_screen(t, 0);
                    t->cur_x = t->sv_x; t->cur_y = t->sv_y;
                    t->cur_fg = t->sv_fg; t->cur_bg = t->sv_bg; t->cur_attr = t->sv_attr;
                }
                t->wrap_pending = 0;
                break;
            default: break;
            }
        }
        /* modos nao-privados (ex.: 4=IRM) ignorados por ora */
    }
}

/* ---- despacho de CSI ---- */

static int cur_top(Terminal *t) { return t->mode_origin ? t->scroll_top : 0; }
static int cur_bot(Terminal *t) { return t->mode_origin ? t->scroll_bot : t->rows - 1; }

static unsigned g_last_cp;   /* para REP (CSI b) */

static void csi_dispatch(Terminal *t, unsigned char final)
{
    int a0 = t->nparams > 0 ? t->params[0] : 0;
    int a1 = t->nparams > 1 ? t->params[1] : 0;
    int p0 = a0 ? a0 : 1;   /* default 1 */
    char buf[32];

    switch (final) {
    case 'A': t->cur_y = clampi(t->cur_y - p0, cur_top(t), t->rows - 1); t->wrap_pending = 0; break;
    case 'B': case 'e': t->cur_y = clampi(t->cur_y + p0, 0, cur_bot(t)); t->wrap_pending = 0; break;
    case 'C': case 'a': t->cur_x = clampi(t->cur_x + p0, 0, t->cols - 1); t->wrap_pending = 0; break;
    case 'D': t->cur_x = clampi(t->cur_x - p0, 0, t->cols - 1); t->wrap_pending = 0; break;
    case 'E': t->cur_x = 0; t->cur_y = clampi(t->cur_y + p0, 0, cur_bot(t)); t->wrap_pending = 0; break;
    case 'F': t->cur_x = 0; t->cur_y = clampi(t->cur_y - p0, cur_top(t), t->rows - 1); t->wrap_pending = 0; break;
    case 'G': case '`': t->cur_x = clampi(p0 - 1, 0, t->cols - 1); t->wrap_pending = 0; break;
    case 'd': t->cur_y = clampi(cur_top(t) + p0 - 1, cur_top(t), cur_bot(t)); t->wrap_pending = 0; break;
    case 'H': case 'f': {
        int row = a0 ? a0 : 1, col = a1 ? a1 : 1;
        t->cur_y = clampi(cur_top(t) + row - 1, cur_top(t), cur_bot(t));
        t->cur_x = clampi(col - 1, 0, t->cols - 1);
        t->wrap_pending = 0;
        break;
    }
    case 'J':   /* ED */
        if (a0 == 2 || a0 == 3) clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
        else if (a0 == 1) { if (t->cur_y > 0) clear_region(t, 0, 0, t->cols - 1, t->cur_y - 1);
                            clear_region(t, 0, t->cur_y, t->cur_x, t->cur_y); }
        else { clear_region(t, t->cur_x, t->cur_y, t->cols - 1, t->cur_y);
               if (t->cur_y < t->rows - 1) clear_region(t, 0, t->cur_y + 1, t->cols - 1, t->rows - 1); }
        break;
    case 'K':   /* EL */
        if (a0 == 2)      clear_region(t, 0, t->cur_y, t->cols - 1, t->cur_y);
        else if (a0 == 1) clear_region(t, 0, t->cur_y, t->cur_x, t->cur_y);
        else              clear_region(t, t->cur_x, t->cur_y, t->cols - 1, t->cur_y);
        break;
    case 'L':   /* IL */
        if (t->cur_y >= t->scroll_top && t->cur_y <= t->scroll_bot) {
            int save = t->scroll_top; t->scroll_top = t->cur_y;
            scroll_down(t, p0); t->scroll_top = save; t->cur_x = 0;
        }
        break;
    case 'M':   /* DL */
        if (t->cur_y >= t->scroll_top && t->cur_y <= t->scroll_bot) {
            int save = t->scroll_top; t->scroll_top = t->cur_y;
            scroll_up(t, p0); t->scroll_top = save; t->cur_x = 0;
        }
        break;
    case '@': {  /* ICH */
        int n = p0; if (n > t->cols - t->cur_x) n = t->cols - t->cur_x;
        for (int x = t->cols - 1; x >= t->cur_x + n; x--)
            t->grid[t->cur_y * t->cols + x] = t->grid[t->cur_y * t->cols + x - n];
        for (int x = t->cur_x; x < t->cur_x + n; x++) set_blank(t, cell_at(t, x, t->cur_y));
        break;
    }
    case 'P': {  /* DCH */
        int n = p0; if (n > t->cols - t->cur_x) n = t->cols - t->cur_x;
        for (int x = t->cur_x; x < t->cols - n; x++)
            t->grid[t->cur_y * t->cols + x] = t->grid[t->cur_y * t->cols + x + n];
        for (int x = t->cols - n; x < t->cols; x++) set_blank(t, cell_at(t, x, t->cur_y));
        break;
    }
    case 'X': {  /* ECH */
        int n = p0; if (n > t->cols - t->cur_x) n = t->cols - t->cur_x;
        for (int x = t->cur_x; x < t->cur_x + n; x++) set_blank(t, cell_at(t, x, t->cur_y));
        break;
    }
    case 'S': scroll_up(t, p0); break;    /* SU */
    case 'T': scroll_down(t, p0); break;  /* SD */
    case 'g':   /* TBC */
        if (a0 == 3) { for (int x = 0; x < t->cols; x++) t->tabs[x] = 0; }
        else if (t->cur_x < t->cols) t->tabs[t->cur_x] = 0;
        break;
    case 'Z': {  /* CBT */
        int n = p0;
        while (n-- > 0) { int x = t->cur_x - 1; while (x > 0 && !t->tabs[x]) x--; t->cur_x = x < 0 ? 0 : x; }
        break;
    }
    case 'm': sgr(t); break;
    case 'h': set_mode(t, 1); break;
    case 'l': set_mode(t, 0); break;
    case 'r':   /* DECSTBM */
        if (t->priv == 0) {
            int top = a0 ? a0 : 1, bot = a1 ? a1 : t->rows;
            if (top < 1) top = 1;
            if (bot > t->rows) bot = t->rows;
            if (top < bot) { t->scroll_top = top - 1; t->scroll_bot = bot - 1;
                             t->cur_x = 0; t->cur_y = cur_top(t); }
        }
        break;
    case 's': t->sv_x = t->cur_x; t->sv_y = t->cur_y;
              t->sv_fg = t->cur_fg; t->sv_bg = t->cur_bg; t->sv_attr = t->cur_attr; break;
    case 'u': t->cur_x = t->sv_x; t->cur_y = t->sv_y;
              t->cur_fg = t->sv_fg; t->cur_bg = t->sv_bg; t->cur_attr = t->sv_attr; break;
    case 'n':   /* DSR */
        if (a0 == 6) { snprintf(buf, sizeof buf, "\x1b[%d;%dR", t->cur_y + 1, t->cur_x + 1); reply(t, buf); }
        else if (a0 == 5) reply(t, "\x1b[0n");
        break;
    case 'c':   /* DA */
        if (t->priv == '>') reply(t, "\x1b[>0;136;0c");
        else                reply(t, "\x1b[?1;2c");
        break;
    default: break;
    }
}

/* ---- decodificacao por byte ---- */

static void reset_csi(Terminal *t)
{
    t->nparams = 0;
    t->param_ovf = 0;
    t->priv = 0;
    t->inter = 0;
    memset(t->params, 0, sizeof t->params);
}

static void osc_end(Terminal *t)
{
    t->osc[t->osc_len < VT_OSCCAP ? t->osc_len : VT_OSCCAP - 1] = 0;
    if (t->str_kind == ']') {
        char *p = t->osc;
        int code = 0, got = 0;
        while (*p >= '0' && *p <= '9') { code = code * 10 + (*p - '0'); p++; got = 1; }
        if (got && *p == ';' && (code == 0 || code == 1 || code == 2)) {
            strncpy(t->title, p + 1, sizeof t->title - 1);
            t->title[sizeof t->title - 1] = 0;
            t->title_changed = 1;
        }
    }
    t->osc_len = 0;
}

static void feed_byte(Terminal *t, unsigned char u)
{
    switch (t->vt_state) {
    case VT_GROUND:
        if (u < 0x20 || u == 0x7f) {
            switch (u) {
            case '\r': t->cur_x = 0; t->wrap_pending = 0; break;
            case '\n': case '\v': case '\f': do_index(t); break;
            case '\b':
                if (t->wrap_pending) t->wrap_pending = 0;
                else if (t->cur_x > 0) t->cur_x--;
                break;
            case '\t': { int x = t->cur_x + 1; while (x < t->cols - 1 && !t->tabs[x]) x++;
                         t->cur_x = x < t->cols ? x : t->cols - 1; break; }
            case 0x1b: t->vt_state = VT_ESC; break;
            default: break;   /* BEL/outros C0/DEL: ignora */
            }
            break;
        }
        /* imprimivel: decodifica UTF-8 */
        if (u < 0x80) {
            t->utf8_left = 0;
            put_cp(t, u); g_last_cp = u;
        } else if ((u & 0xe0) == 0xc0) { t->utf8_cp = u & 0x1f; t->utf8_left = 1; }
        else if ((u & 0xf0) == 0xe0)   { t->utf8_cp = u & 0x0f; t->utf8_left = 2; }
        else if ((u & 0xf8) == 0xf0)   { t->utf8_cp = u & 0x07; t->utf8_left = 3; }
        else if ((u & 0xc0) == 0x80) {   /* continuacao */
            if (t->utf8_left > 0) {
                t->utf8_cp = (t->utf8_cp << 6) | (u & 0x3f);
                if (--t->utf8_left == 0) { put_cp(t, t->utf8_cp); g_last_cp = t->utf8_cp; }
            }
        } else { t->utf8_left = 0; put_cp(t, '?'); }
        break;

    case VT_ESC:
        t->utf8_left = 0;
        switch (u) {
        case '[': t->vt_state = VT_CSI; reset_csi(t); break;
        case ']': t->vt_state = VT_OSC; t->str_kind = ']'; t->osc_len = 0; break;
        case 'P': case '^': case '_': t->vt_state = VT_STR; t->str_kind = (char)u; break;
        case '(': case ')': case '*': case '+':
            t->vt_state = VT_ESC_INT; break;
        case 'c':   /* RIS */
            t->cur_x = t->cur_y = 0; t->wrap_pending = 0;
            grid_reset_defaults(t);
            t->scroll_top = 0; t->scroll_bot = t->rows - 1;
            switch_screen(t, 0);
            clear_region(t, 0, 0, t->cols - 1, t->rows - 1);
            t->vt_state = VT_GROUND; break;
        case '7': t->sv_x = t->cur_x; t->sv_y = t->cur_y;   /* DECSC */
                  t->sv_fg = t->cur_fg; t->sv_bg = t->cur_bg; t->sv_attr = t->cur_attr;
                  t->vt_state = VT_GROUND; break;
        case '8': t->cur_x = t->sv_x; t->cur_y = t->sv_y;   /* DECRC */
                  t->cur_fg = t->sv_fg; t->cur_bg = t->sv_bg; t->cur_attr = t->sv_attr;
                  t->wrap_pending = 0; t->vt_state = VT_GROUND; break;
        case 'D': do_index(t); t->vt_state = VT_GROUND; break;    /* IND */
        case 'M': do_rindex(t); t->vt_state = VT_GROUND; break;   /* RI */
        case 'E': t->cur_x = 0; do_index(t); t->vt_state = VT_GROUND; break;  /* NEL */
        case 'H': if (t->cur_x < t->cols) t->tabs[t->cur_x] = 1; t->vt_state = VT_GROUND; break; /* HTS */
        case '=': case '>': t->vt_state = VT_GROUND; break;       /* keypad */
        default: t->vt_state = VT_GROUND; break;
        }
        break;

    case VT_ESC_INT:   /* consome o byte final da selecao de charset */
        t->vt_state = VT_GROUND;
        break;

    case VT_CSI:
    case VT_CSI_INT:
        if (u >= '0' && u <= '9') {
            if (t->nparams == 0) t->nparams = 1;
            if (t->nparams <= VT_MAXPARAMS) {
                int *p = &t->params[t->nparams - 1];
                if (*p > 214748364) t->param_ovf = 1;   /* #108 */
                else *p = *p * 10 + (u - '0');
            }
        } else if (u == ';' || u == ':') {
            if (t->nparams < VT_MAXPARAMS) t->nparams++;   /* #109: extras nao sobrescrevem */
        } else if (t->vt_state == VT_CSI && (u == '?' || u == '>' || u == '!' || u == '=')) {
            t->priv = (char)u;
        } else if (u >= 0x20 && u <= 0x2f) {
            t->inter = (char)u; t->vt_state = VT_CSI_INT;
        } else if (u >= 0x40 && u <= 0x7e) {
            if (u == 'b') {   /* REP */
                int rep = t->nparams > 0 && t->params[0] ? t->params[0] : 1;
                if (!t->param_ovf) while (rep-- > 0) put_cp(t, g_last_cp);
            } else if (!t->param_ovf) {
                csi_dispatch(t, u);
            }
            t->vt_state = VT_GROUND;
        } else {
            t->vt_state = VT_GROUND;
        }
        break;

    case VT_OSC:
        if (u == 0x07) { osc_end(t); t->vt_state = VT_GROUND; }
        else if (u == 0x1b) { osc_end(t); t->vt_state = VT_ESC; }   /* ST = ESC \ */
        else if (t->osc_len < VT_OSCCAP - 1) t->osc[t->osc_len++] = (char)u;
        break;

    case VT_STR:   /* DCS/PM/APC: ignora ate ST */
        if (u == 0x1b) t->vt_state = VT_ESC;
        else if (u == 0x07) t->vt_state = VT_GROUND;
        break;
    }
}

void vt_feed(Terminal *t, const char *bytes, int n)
{
    char rbuf[VT_REPLYCAP];
    int rlen;

    EnterCriticalSection(&t->lock);
    for (int i = 0; i < n; i++)
        feed_byte(t, (unsigned char)bytes[i]);
    t->dirty = 1;
    rlen = t->reply_len;
    if (rlen > 0) { memcpy(rbuf, t->reply, (size_t)rlen); t->reply_len = 0; }
    LeaveCriticalSection(&t->lock);

    InterlockedExchangeAdd(&t->rx, n);

    /* respostas a queries (DSR/DA): fora do lock, so no pty real */
    if (rlen > 0 && t->backend_is_pty && t->be && t->be->input)
        t->be->input(t, rbuf, rlen);
}

/* ---- render da grade no DIB (main thread) ---- */

static Cell *g_snap;
static size_t g_snap_cap;

/* codepoint -> byte renderavel na fonte OEM (CP437). ASCII direto; box-drawing/
 * blocos comuns mapeados; resto '?'. */
static unsigned char cp_to_oem(unsigned cp)
{
    if (cp == 0) return ' ';
    if (cp >= 0x20 && cp < 0x7f) return (unsigned char)cp;
    switch (cp) {
    case 0x2500: return 0xC4; case 0x2502: return 0xB3;
    case 0x250C: return 0xDA; case 0x2510: return 0xBF;
    case 0x2514: return 0xC0; case 0x2518: return 0xD9;
    case 0x251C: return 0xC3; case 0x2524: return 0xB4;
    case 0x252C: return 0xC2; case 0x2534: return 0xC1;
    case 0x253C: return 0xC5;
    case 0x2591: return 0xB0; case 0x2592: return 0xB1;
    case 0x2593: return 0xB2; case 0x2588: return 0xDB;
    case 0x2580: return 0xDF; case 0x2584: return 0xDC;
    case 0x25CF: return 0x07; case 0x2022: return 0x07;
    case 0x2190: return 0x1B; case 0x2192: return 0x1A;
    case 0x2191: return 0x18; case 0x2193: return 0x19;
    case 0x00A0: return ' ';
    }
    if (cp < 0x100) return (unsigned char)cp;
    return '?';
}

static COLORREF brighten(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    r += (255 - r) / 3; g += (255 - g) / 3; b += (255 - b) / 3;
    return RGB(r, g, b);
}

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
    else { LeaveCriticalSection(&t->lock); return; }
    t->dirty = 0;
    LeaveCriticalSection(&t->lock);

    HFONT old = (HFONT)SelectObject(memdc, font);
    SetBkMode(memdc, OPAQUE);

    for (int y = 0; y < rows; y++) {
        int x = 0;
        while (x < cols) {
            Cell *c0 = &g_snap[y * cols + x];
            unsigned short a0 = c0->attr;
            COLORREF fg0 = c0->fg, bg0 = c0->bg;
            int run = 1;
            while (x + run < cols) {
                Cell *cn = &g_snap[y * cols + x + run];
                if (cn->fg != fg0 || cn->bg != bg0 || cn->attr != a0)
                    break;
                run++;
            }
            COLORREF fg = fg0, bg = bg0;
            if (a0 & ATTR_BOLD)    fg = brighten(fg);
            if (a0 & ATTR_REVERSE) { COLORREF tmp = fg; fg = bg; bg = tmp; }
            if (a0 & ATTR_INVIS)   fg = bg;
            SetTextColor(memdc, fg);
            SetBkColor(memdc, bg);

            char buf[256];
            int bn = run < (int)sizeof buf ? run : (int)sizeof buf - 1;
            for (int i = 0; i < bn; i++) {
                Cell *cc = &g_snap[y * cols + x + i];
                buf[i] = (cc->attr & ATTR_WDUMMY) ? ' ' : (char)cp_to_oem(cc->ch);
            }
            RECT rc = { x * cellw, y * cellh, (x + bn) * cellw, (y + 1) * cellh };
            ExtTextOutA(memdc, x * cellw, y * cellh, ETO_OPAQUE | ETO_CLIPPED,
                        &rc, buf, bn, NULL);
            if (a0 & ATTR_UNDERLINE) {
                HPEN pen = CreatePen(PS_SOLID, 1, fg);
                HPEN op = (HPEN)SelectObject(memdc, pen);
                MoveToEx(memdc, x * cellw, (y + 1) * cellh - 1, NULL);
                LineTo(memdc, (x + bn) * cellw, (y + 1) * cellh - 1);
                SelectObject(memdc, op);
                DeleteObject(pen);
            }
            x += bn;
        }
    }

    if (cvis && cx < cols && cy < rows) {
        RECT rc = { cx * cellw, cy * cellh, (cx + 1) * cellw, (cy + 1) * cellh };
        InvertRect(memdc, &rc);
    }

    SelectObject(memdc, old);
}
