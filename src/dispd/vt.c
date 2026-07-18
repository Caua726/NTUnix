/*
 * vt.c — glue com o libvterm (emulador VT) + render da grade no DIB.
 *
 * O parsing/estado VT e' 100% do libvterm (third_party/libvterm), a engine do
 * vim/neovim: responde DSR/CPR/DA sozinho (era a causa do ash preto), trata
 * alt-screen, scroll region, mouse, 256/truecolor, UTF-8. Aqui so:
 *   - alimentamos bytes do pty (vt_feed -> vterm_input_write);
 *   - roteamos a saida do libvterm de volta ao pty (callback -> t->reply);
 *   - lemos a grade de celulas (vt_render -> vterm_screen_get_cell) e desenhamos
 *     com GDI (fonte OEM/CP437: ASCII direto, box-drawing mapeado, resto '?').
 *
 * O fallback scrape nao usa libvterm — escreve numa grade Cell propria (t->grid);
 * vt_render trata os dois casos.
 */
#include "term.h"
#include "vterm.h"
#include "../common/ntuwm.h"   /* MOD_* p/ o mapeamento de mods do mouse (#21) */

int g_vt_cursor_on = 1;                        /* audit #115: fase do piscar do cursor */
void vt_cursor_tick(void) { g_vt_cursor_on = !g_vt_cursor_on; }

#define DEF_FG  RGB(0xE5, 0xE5, 0xE5)
#define DEF_BG  RGB(0x00, 0x00, 0x00)

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

/* ---- callbacks do libvterm (rodam sob t->lock, dentro de vterm_input_write) ---- */

static void out_cb(const char *s, size_t len, void *user)
{
    Terminal *t = (Terminal *)user;
    if (t->reply_len + (int)len > (int)sizeof t->reply)
        return;   /* buffer cheio: descarta (respostas sao pequenas) */
    memcpy(t->reply + t->reply_len, s, len);
    t->reply_len += (int)len;
}

static int cb_damage(VTermRect r, void *user)
{
    (void)r;
    ((Terminal *)user)->dirty = 1;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos old, int visible, void *user)
{
    Terminal *t = (Terminal *)user;
    (void)old;
    t->cur_x = pos.col;
    t->cur_y = pos.row;
    t->cur_vis = visible;
    t->dirty = 1;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Terminal *t = (Terminal *)user;
    if (prop == VTERM_PROP_TITLE) {
        VTermStringFragment f = val->string;
        if (f.initial)
            t->title_len = 0;
        for (size_t i = 0; i < f.len && t->title_len < (int)sizeof t->title - 1; i++) {
            unsigned char c = (unsigned char)f.str[i];
            t->title[t->title_len++] = (c < 0x20) ? ' ' : (char)c;   /* sem controle */
        }
        if (f.final) {
            t->title[t->title_len] = 0;
            t->title_changed = 1;
        }
    } else if (prop == VTERM_PROP_CURSORVISIBLE) {
        t->cur_vis = val->boolean;
        t->dirty = 1;
    } else if (prop == VTERM_PROP_ALTSCREEN) {
        t->on_alt = val->boolean;   /* tela alt (vim/htop): sem scrollback */
        t->scroll_off = 0;
        t->dirty = 1;
    } else if (prop == VTERM_PROP_CURSORSHAPE) {   /* audit #99: DECSCUSR */
        t->cur_shape = val->number;   /* 1=bloco, 2=underline, 3=barra */
        t->dirty = 1;
    }
    return 1;
}

static COLORREF vcolor(VTermScreen *vts, VTermColor c, COLORREF def);

/* uma linha rolou pra fora do topo -> guarda no ring de scrollback */
static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    Terminal *t = (Terminal *)user;
    if (!t->sb || t->sb_cap <= 0)
        return 0;
    SBLine *ln = &t->sb[t->sb_head];   /* recicla a linha mais antiga do ring */
    Cell *nc = (Cell *)realloc(ln->cells, (size_t)cols * sizeof(Cell));
    if (!nc)
        return 1;                       /* sem memoria: descarta, mas segue */
    ln->cells = nc;
    ln->len = cols;
    for (int x = 0; x < cols; x++) {
        VTermScreenCell c = cells[x];
        ln->cells[x].ch = c.chars[0] ? c.chars[0] : ' ';
        ln->cells[x].w = (unsigned char)c.width;   /* #98 */
        ln->cells[x].fg = vcolor((VTermScreen *)t->vts, c.fg, DEF_FG);
        ln->cells[x].bg = vcolor((VTermScreen *)t->vts, c.bg, DEF_BG);
        ln->cells[x].attr = (unsigned short)((c.attrs.bold ? ATTR_BOLD : 0) |
                                             (c.attrs.reverse ? ATTR_REVERSE : 0) |
                                             (c.attrs.underline ? ATTR_UNDERLINE : 0));
    }
    t->sb_head = (t->sb_head + 1) % t->sb_cap;
    if (t->sb_count < t->sb_cap)
        t->sb_count++;
    if (t->scroll_off > 0 && t->scroll_off < t->sb_count)
        t->scroll_off++;                /* mantem a posicao ao rolar historico */
    t->dirty = 1;
    return 1;
}

static const VTermScreenCallbacks g_screen_cbs = {
    .damage      = cb_damage,
    .moverect    = NULL,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell        = NULL,
    .resize      = NULL,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = NULL,
};

/* ---- ciclo de vida ---- */

int vt_init(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    t->cols = cols;
    t->rows = rows;
    t->cur_x = t->cur_y = 0;
    t->cur_vis = 1;
    t->vt = NULL;
    t->vts = NULL;
    /* grade do fallback scrape (o libvterm tem a sua propria) */
    t->grid = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
    if (!t->grid)
        return -1;
    for (int i = 0; i < cols * rows; i++) {
        t->grid[i].ch = ' '; t->grid[i].fg = DEF_FG; t->grid[i].bg = DEF_BG;
    }
    return 0;
}

/* liga o libvterm — chamado pelo backend ConPTY (stream VT real) */
int vt_use_libvterm(Terminal *t)
{
    VTerm *vt = vterm_new(t->rows, t->cols);
    if (!vt)
        return -1;
    vterm_set_utf8(vt, 1);
    vterm_output_set_callback(vt, out_cb, t);
    VTermScreen *vts = vterm_obtain_screen(vt);
    if (!vts) {   /* audit #94: OOM interno do libvterm -> rollback completo */
        vterm_free(vt);
        return -1;
    }
    vterm_screen_set_callbacks(vts, &g_screen_cbs, t);
    vterm_screen_enable_altscreen(vts, 1);
    vterm_screen_reset(vts, 1);   /* hard reset */
    t->vt = vt;
    t->vts = vts;
    /* scrollback: ring de linhas (falha vira "sem scrollback", nao fatal) */
    t->sb_cap = 2000;
    t->sb = (SBLine *)calloc((size_t)t->sb_cap, sizeof(SBLine));
    if (!t->sb)
        t->sb_cap = 0;
    return 0;
}

void vt_free(Terminal *t)
{
    if (t->vt) {
        vterm_free((VTerm *)t->vt);
        t->vt = NULL;
        t->vts = NULL;
    }
    if (t->sb) {
        for (int i = 0; i < t->sb_cap; i++)
            free(t->sb[i].cells);
        free(t->sb);
        t->sb = NULL;
    }
    t->sb_cap = t->sb_count = t->sb_head = t->scroll_off = 0;
    free(t->grid);
    t->grid = NULL;
}

/* audit #66: solta SO a parte libvterm (mantem a grade scrape) — usado entre
 * tentativas de backend: se o pty liga o libvterm e falha, o scrape seguinte NAO
 * pode herdar t->vt (senao renderiza como libvterm e o mouse pega o caminho errado). */
void vt_drop_libvterm(Terminal *t)
{
    if (t->vt) {
        vterm_free((VTerm *)t->vt);
        t->vt = NULL;
        t->vts = NULL;
    }
    if (t->sb) {
        for (int i = 0; i < t->sb_cap; i++)
            free(t->sb[i].cells);
        free(t->sb);
        t->sb = NULL;
    }
    t->sb_cap = t->sb_count = t->sb_head = t->scroll_off = 0;
    t->on_alt = 0;
    t->backend_is_pty = 0;
}

void vt_scroll(Terminal *t, int delta_lines)
{
    if (!t)
        return;
    EnterCriticalSection(&t->lock);
    if (!t->on_alt) {
        t->scroll_off += delta_lines;
        if (t->scroll_off < 0) t->scroll_off = 0;
        if (t->scroll_off > t->sb_count) t->scroll_off = t->sb_count;
        t->dirty = 1;
    }
    LeaveCriticalSection(&t->lock);
}

void vt_scroll_reset(Terminal *t)
{
    if (!t)
        return;
    EnterCriticalSection(&t->lock);
    if (t->scroll_off) { t->scroll_off = 0; t->dirty = 1; }
    LeaveCriticalSection(&t->lock);
}

void vt_resize(Terminal *t, int cols, int rows)
{
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows)
        return;
    if (t->vt) {
        vterm_set_size((VTerm *)t->vt, rows, cols);
    } else {
        /* audit #93 (critico): so publica cols/rows novos se a grade nova
         * alocou. Antes, calloc falho deixava a grade ANTIGA (menor) com
         * cols/rows NOVOS (maiores) -> OOB no reader/render. Em OOM, mantem
         * dimensoes e grade antigas (coerentes). */
        Cell *ng = (Cell *)calloc((size_t)cols * rows, sizeof(Cell));
        if (!ng)
            return;
        for (int i = 0; i < cols * rows; i++) {
            ng[i].ch = ' '; ng[i].fg = DEF_FG; ng[i].bg = DEF_BG;
        }
        int cx = t->cols < cols ? t->cols : cols;
        int cy = t->rows < rows ? t->rows : rows;
        for (int y = 0; y < cy; y++)
            for (int x = 0; x < cx; x++)
                ng[y * cols + x] = t->grid[y * t->cols + x];
        free(t->grid);
        t->grid = ng;
    }
    t->cols = cols;
    t->rows = rows;
    if (t->cur_x >= cols) t->cur_x = cols - 1;
    if (t->cur_y >= rows) t->cur_y = rows - 1;
}

void vt_feed(Terminal *t, const char *bytes, int n)
{
    char rbuf[sizeof t->reply];
    int rlen;

    EnterCriticalSection(&t->lock);
    if (t->vt)
        vterm_input_write((VTerm *)t->vt, bytes, (size_t)n);
    t->dirty = 1;
    rlen = t->reply_len;
    if (rlen > 0) { memcpy(rbuf, t->reply, (size_t)rlen); t->reply_len = 0; }
    LeaveCriticalSection(&t->lock);

    InterlockedExchangeAdd(&t->rx, n);

    /* respostas do libvterm (DSR/DA/mouse) de volta ao pty, fora do lock */
    if (rlen > 0 && t->backend_is_pty && t->be && t->be->input)
        t->be->input(t, rbuf, rlen);
}

/* encaminha mouse ao pty via libvterm (ele encoda conforme o modo do app; se o
 * app nao pediu rastreio, nao emite nada) */
void term_mouse(Terminal *t, int col, int row, int button, int press, int motion, unsigned mods)
{
    if (!t || !t->vt)
        return;
    char rbuf[sizeof t->reply];
    int rlen;

    VTermModifier vmod = VTERM_MOD_NONE;       /* audit #21: propaga Shift/Ctrl/Alt */
    if (mods & MOD_SHIFT) vmod |= VTERM_MOD_SHIFT;
    if (mods & MOD_CTRL)  vmod |= VTERM_MOD_CTRL;
    if (mods & MOD_ALT)   vmod |= VTERM_MOD_ALT;

    EnterCriticalSection(&t->lock);
    VTerm *vt = (VTerm *)t->vt;
    if (motion) {
        vterm_mouse_move(vt, row, col, vmod);
    } else if (button >= 64) {                 /* roda: libvterm botoes 4/5 */
        vterm_mouse_button(vt, button == 64 ? 4 : 5, 1, vmod);
    } else {                                    /* 0/1/2 -> libvterm 1/2/3 */
        vterm_mouse_button(vt, button + 1, press ? 1 : 0, vmod);
    }
    rlen = t->reply_len;
    if (rlen > 0) { memcpy(rbuf, t->reply, (size_t)rlen); t->reply_len = 0; }
    LeaveCriticalSection(&t->lock);

    if (rlen > 0 && t->backend_is_pty && t->be && t->be->input)
        t->be->input(t, rbuf, rlen);
}

/* audit #16: encoda tecla especial via libvterm (respeita DECCKM/application
 * cursor, keypad mode, encoding de modificadores) em vez de sequencia fixa.
 * Recebe o VK do Windows; 1 = tratou, 0 = nao e' tecla especial (cai no texto). */
int vt_key(Terminal *t, int vk, unsigned mods)
{
    if (!t || !t->vt)
        return 0;
    VTermKey key = VTERM_KEY_NONE;
    switch (vk) {
    case VK_UP:     key = VTERM_KEY_UP;       break;
    case VK_DOWN:   key = VTERM_KEY_DOWN;     break;
    case VK_LEFT:   key = VTERM_KEY_LEFT;     break;
    case VK_RIGHT:  key = VTERM_KEY_RIGHT;    break;
    case VK_HOME:   key = VTERM_KEY_HOME;     break;
    case VK_END:    key = VTERM_KEY_END;      break;
    case VK_PRIOR:  key = VTERM_KEY_PAGEUP;   break;
    case VK_NEXT:   key = VTERM_KEY_PAGEDOWN; break;
    case VK_INSERT: key = VTERM_KEY_INS;      break;
    case VK_DELETE: key = VTERM_KEY_DEL;      break;
    case VK_RETURN: key = VTERM_KEY_ENTER;    break;
    case VK_BACK:   key = VTERM_KEY_BACKSPACE; break;
    case VK_ESCAPE: key = VTERM_KEY_ESCAPE;   break;
    case VK_TAB:    key = VTERM_KEY_TAB;      break;
    default:
        if (vk >= VK_F1 && vk <= VK_F24)
            key = (VTermKey)(VTERM_KEY_FUNCTION(1) + (vk - VK_F1));
        break;
    }
    if (key == VTERM_KEY_NONE)
        return 0;

    VTermModifier vmod = VTERM_MOD_NONE;
    if (mods & MOD_SHIFT) vmod |= VTERM_MOD_SHIFT;
    if (mods & MOD_CTRL)  vmod |= VTERM_MOD_CTRL;
    if (mods & MOD_ALT)   vmod |= VTERM_MOD_ALT;

    char rbuf[sizeof t->reply];
    int rlen;
    EnterCriticalSection(&t->lock);
    vterm_keyboard_key((VTerm *)t->vt, key, vmod);
    rlen = t->reply_len;
    if (rlen > 0) { memcpy(rbuf, t->reply, (size_t)rlen); t->reply_len = 0; }
    LeaveCriticalSection(&t->lock);
    if (rlen > 0 && t->be && t->be->input)
        t->be->input(t, rbuf, rlen);
    return 1;
}

/* ---- render ---- */

static Cell *g_snap;
static size_t g_snap_cap;

static COLORREF brighten(COLORREF c)
{
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    r += (255 - r) / 3; g += (255 - g) / 3; b += (255 - b) / 3;
    return RGB(r, g, b);
}

/* VTermColor -> COLORREF (default -> def; indexado -> converte pela paleta) */
static COLORREF vcolor(VTermScreen *vts, VTermColor c, COLORREF def)
{
    if (c.type & VTERM_COLOR_DEFAULT_MASK)
        return def;
    if (VTERM_COLOR_IS_INDEXED(&c))
        vterm_screen_convert_color_to_rgb(vts, &c);
    return RGB(c.rgb.red, c.rgb.green, c.rgb.blue);
}

void vt_render(Terminal *t, HDC memdc, HFONT font, int cellw, int cellh)
{
    int cols, rows, cx, cy, cvis, soff;

    EnterCriticalSection(&t->lock);
    cols = t->cols; rows = t->rows;
    cx = t->cur_x; cy = t->cur_y; cvis = t->cur_vis;
    soff = t->scroll_off;
    size_t need = (size_t)cols * rows * sizeof(Cell);
    if (need > g_snap_cap) {
        Cell *ns = (Cell *)realloc(g_snap, need);
        if (ns) { g_snap = ns; g_snap_cap = need; }
    }
    if (!g_snap || need > g_snap_cap) { LeaveCriticalSection(&t->lock); return; }

    if (t->vt) {
        VTermScreen *vts = (VTermScreen *)t->vts;
        int sbn = t->sb_count;
        for (int y = 0; y < rows; y++) {
            /* viewport: [scrollback (0=mais antiga) .. tela viva], rolada soff */
            int comb = sbn - soff + y;
            if (t->sb && comb >= 0 && comb < sbn) {
                int ring = (t->sb_head - sbn + comb + t->sb_cap) % t->sb_cap;
                SBLine *ln = &t->sb[ring];
                for (int x = 0; x < cols; x++) {
                    Cell *o = &g_snap[y * cols + x];
                    if (ln->cells && x < ln->len) *o = ln->cells[x];
                    else { o->ch = ' '; o->fg = DEF_FG; o->bg = DEF_BG; o->attr = 0; }
                }
                continue;
            }
            int ly = comb - sbn;   /* linha da tela viva */
            for (int x = 0; x < cols; x++) {
                Cell *o = &g_snap[y * cols + x];
                VTermPos p; p.row = ly; p.col = x;
                VTermScreenCell vc;
                if (ly < 0 || ly >= rows || !vterm_screen_get_cell(vts, p, &vc)) {
                    o->ch = ' '; o->fg = DEF_FG; o->bg = DEF_BG; o->attr = 0;
                    continue;
                }
                o->ch = vc.chars[0] ? vc.chars[0] : ' ';
                o->w = (unsigned char)vc.width;   /* #98: 1 ou 2 (CJK/wide) */
                o->fg = vcolor(vts, vc.fg, DEF_FG);
                o->bg = vcolor(vts, vc.bg, DEF_BG);
                o->attr = (unsigned short)((vc.attrs.bold ? ATTR_BOLD : 0) |
                                           (vc.attrs.reverse ? ATTR_REVERSE : 0) |
                                           (vc.attrs.underline ? ATTR_UNDERLINE : 0));
            }
        }
    } else {
        memcpy(g_snap, t->grid, need);
    }
    t->dirty = 0;
    LeaveCriticalSection(&t->lock);

    HFONT old = (HFONT)SelectObject(memdc, font);
    SetBkMode(memdc, OPAQUE);

    for (int y = 0; y < rows; y++) {
        int x = 0;
        while (x < cols) {
            Cell *c0 = &g_snap[y * cols + x];
            int w0 = c0->w ? c0->w : 1;   /* #98: largura em celulas (0 = 1) */
            unsigned short a0 = c0->attr;
            COLORREF fg0 = c0->fg, bg0 = c0->bg;
            /* run de celulas LARGURA-1 com mesma cor/attr; um wide char (w0>1) e'
             * desenhado SOZINHO — o run para antes dele e a celula spacer seguinte
             * e' pulada (x += span). Igual xterm/kitty. */
            int run = 1;
            if (w0 == 1)
                while (x + run < cols) {
                    Cell *cn = &g_snap[y * cols + x + run];
                    if ((cn->w ? cn->w : 1) != 1 ||
                        cn->fg != fg0 || cn->bg != bg0 || cn->attr != a0)
                        break;
                    run++;
                }
            COLORREF fg = fg0, bg = bg0;
            if (a0 & ATTR_BOLD)    fg = brighten(fg);
            if (a0 & ATTR_REVERSE) { COLORREF tmp = fg; fg = bg; bg = tmp; }
            SetTextColor(memdc, fg);
            SetBkColor(memdc, bg);

            WCHAR wbuf[256];
            int dx[256];
            int bn = run < 256 ? run : 255;
            for (int i = 0; i < bn; i++) {
                unsigned cp = g_snap[y * cols + x + i].ch;
                wbuf[i] = (cp >= 0x20 && cp < 0x10000) ? (WCHAR)cp
                        : (cp >= 0x10000 ? (WCHAR)0xFFFD : L' ');
                dx[i] = (i == 0 ? w0 : 1) * cellw;   /* #98: wide char ocupa w0 celulas */
            }
            int span = (w0 == 1) ? bn : w0;   /* celulas cobertas por este desenho */
            RECT rc = { x * cellw, y * cellh, (x + span) * cellw, (y + 1) * cellh };
            ExtTextOutW(memdc, x * cellw, y * cellh, ETO_OPAQUE | ETO_CLIPPED,
                        &rc, wbuf, bn, dx);
            if (a0 & ATTR_UNDERLINE) {
                HPEN pen = CreatePen(PS_SOLID, 1, fg);
                HPEN op = (HPEN)SelectObject(memdc, pen);
                MoveToEx(memdc, x * cellw, (y + 1) * cellh - 1, NULL);
                LineTo(memdc, (x + span) * cellw, (y + 1) * cellh - 1);
                SelectObject(memdc, op);
                DeleteObject(pen);
            }
            x += span;
        }
    }

    /* audit #115: cursor PISCA de verdade — g_vt_cursor_on e' alternado pelo frame
     * loop (vt_cursor_tick), que tambem marca o terminal focado sujo. Assim cada
     * alternancia = 1 recompose (sincronizado, sem amostragem defasada). */
    if (cvis && g_vt_cursor_on && !soff && cx < cols && cy < rows && cx >= 0 && cy >= 0) {
        RECT rc = { cx * cellw, cy * cellh, (cx + 1) * cellw, (cy + 1) * cellh };
        if (t->cur_shape == 2)          /* audit #99: underline */
            rc.top = rc.bottom - 2;
        else if (t->cur_shape == 3)     /* barra vertical */
            rc.right = rc.left + 2;
        InvertRect(memdc, &rc);         /* bloco (0/1) = celula inteira */
    }

    SelectObject(memdc, old);
}
