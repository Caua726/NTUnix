/*
 * term.c — fabrica de terminais: escolhe o backend (ConPTY -> scrape) e
 * roteia input/resize/close. O parser+grade (vt.c) e' comum aos backends.
 */
#include "term.h"

Terminal *term_create(const char *cmdline, int cols, int rows, struct Window *win)
{
    Terminal *t = (Terminal *)calloc(1, sizeof *t);
    if (!t)
        return NULL;
    InitializeCriticalSection(&t->lock);
    if (vt_init(t, cols, rows) != 0) {   /* #105: grade nula nao vira terminal */
        DeleteCriticalSection(&t->lock);
        free(t);
        return NULL;
    }
    t->win = win;
    t->alive = 1;

    /* ConPTY primeiro; se indisponivel (ex.: WinPE sem CreatePseudoConsole),
     * cai no scrape (console oculto + ReadConsoleOutput). */
    t->be = &term_conpty_backend;
    if (t->be->start(t, cmdline, cols, rows) != 0) {
        t->be = &term_scrape_backend;
        if (t->be->start(t, cmdline, cols, rows) != 0) {
            vt_free(t);
            DeleteCriticalSection(&t->lock);
            free(t);
            return NULL;
        }
    }
    return t;
}

void term_destroy(Terminal *t)
{
    if (!t)
        return;
    if (t->be && t->be->close)
        t->be->close(t);
    vt_free(t);
    DeleteCriticalSection(&t->lock);
    free(t);
}

void term_input(Terminal *t, const char *bytes, int n)
{
    if (t && t->be && t->be->input)
        t->be->input(t, bytes, n);
}

/* encaminha mouse ao pty conforme o modo de rastreio negociado pelo programa
 * (?1000 normal, ?1002 button-event, ?1003 any-event; ?1006 encode SGR). */
void term_mouse(Terminal *t, int col, int row, int button, int press, int motion)
{
    if (!t || !t->backend_is_pty || t->mode_mouse == 0)
        return;
    int mm = t->mode_mouse;
    if (motion) {
        /* 1003(any)=bit 0x4; 1002(button)=bit 0x2 e so com botao pressionado */
        if (!(mm & 0x4) && !((mm & 0x2) && button >= 0))
            return;
    }
    if (col < 0) col = 0;
    if (row < 0) row = 0;
    char seq[32];
    int n;
    if (t->mode_mouse_sgr) {              /* ?1006: ESC[<b;x;y(M|m) */
        int b = (button < 0) ? 0 : button;
        if (motion) b += 32;
        n = snprintf(seq, sizeof seq, "\x1b[<%d;%d;%d%c", b, col + 1, row + 1,
                     press ? 'M' : 'm');
    } else {                             /* legado: ESC[M cb cx cy (bytes) */
        int b = press ? (button < 0 ? 0 : button) : (button >= 64 ? button : 3);
        if (motion) b += 32;
        int cx = col + 1, cy = row + 1;
        if (cx > 223) cx = 223;
        if (cy > 223) cy = 223;
        seq[0] = 0x1b; seq[1] = '['; seq[2] = 'M';
        seq[3] = (char)(32 + b); seq[4] = (char)(32 + cx); seq[5] = (char)(32 + cy);
        n = 6;
    }
    if (t->be && t->be->input && n > 0)
        t->be->input(t, seq, n);
}

void term_resize(Terminal *t, int cols, int rows)
{
    if (!t)
        return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    EnterCriticalSection(&t->lock);
    vt_resize(t, cols, rows);
    t->dirty = 1;              /* #12: re-renderiza apos resize (evita ficar preto) */
    LeaveCriticalSection(&t->lock);
    if (t->be && t->be->resize)
        t->be->resize(t, cols, rows);
}
