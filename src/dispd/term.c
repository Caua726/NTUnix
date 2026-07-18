/*
 * term.c — fabrica de terminais: escolhe o backend (ConPTY -> scrape) e
 * roteia input/resize/close. O parser+grade (vt.c) e' comum aos backends.
 *
 * DISPD_TERM seleciona o backend p/ diagnostico: "conpty" (so ConPTY), "scrape"
 * (so console real), "demo" (alimenta o libvterm com um stream VT fixo — valida
 * render/janela sem depender do pty). Default: ConPTY com fallback scrape.
 */
#include "term.h"

/* backend de diagnostico: sem processo filho; injeta um stream VT canonico no
 * libvterm pra validar o pipeline de render (janela/GDI/present) isoladamente. */
static int demo_start(Terminal *t, const char *cmdline, int cols, int rows)
{
    (void)cmdline; (void)cols; (void)rows;
    if (vt_use_libvterm(t) != 0)
        return -1;
    t->backend_is_pty = 0;   /* sem pty: nao ha write-back de respostas */
    static const char s[] =
        "\x1b[1;32mNTUnix\x1b[0m compositor + libvterm (demo)\r\n"
        "\x1b[33mamarelo\x1b[0m \x1b[31mvermelho\x1b[0m \x1b[36mciano\x1b[0m "
        "\x1b[44m fundo azul \x1b[0m \x1b[4msublinhado\x1b[0m\r\n"
        "\x1b[38;2;255;128;0mtruecolor laranja\x1b[0m\r\n"
        "/ # echo teste\r\nteste\r\n/ # ";
    vt_feed(t, s, (int)(sizeof s - 1));
    t->alive = 1;
    return 0;
}
static TerminalBackend term_demo_backend = { "demo", demo_start, NULL, NULL, NULL };

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

    char sel[16] = "";
    GetEnvironmentVariableA("DISPD_TERM", sel, sizeof sel);

    /* backends candidatos conforme DISPD_TERM (NULL termina a lista) */
    TerminalBackend *chain[3] = { NULL, NULL, NULL };
    if (!_stricmp(sel, "demo"))        chain[0] = &term_demo_backend;
    else if (!_stricmp(sel, "scrape")) chain[0] = &term_scrape_backend;
    else if (!_stricmp(sel, "conpty")) chain[0] = &term_conpty_backend;
    else if (!_stricmp(sel, "pty"))    chain[0] = &term_pty_backend;
    /* default: pty nativo (musl-nt) — sem ConPTY, multi-terminal, stream VT limpo
     * pro libvterm; scrape de fallback (1 terminal) se o pty falhar. DISPD_TERM
     * forca um backend especifico (pty|scrape|conpty|demo). */
    else { chain[0] = &term_pty_backend; chain[1] = &term_scrape_backend; }

    for (int i = 0; i < 3 && chain[i]; i++) {
        t->be = chain[i];
        if (t->be->start(t, cmdline, cols, rows) == 0)
            return t;
    }
    vt_free(t);
    DeleteCriticalSection(&t->lock);
    free(t);
    return NULL;
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

/* term_mouse esta em vt.c (usa o encoder de mouse do libvterm) */

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
