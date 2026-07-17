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
    vt_init(t, cols, rows);
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

void term_resize(Terminal *t, int cols, int rows)
{
    if (!t)
        return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    EnterCriticalSection(&t->lock);
    vt_resize(t, cols, rows);
    LeaveCriticalSection(&t->lock);
    if (t->be && t->be->resize)
        t->be->resize(t, cols, rows);
}
