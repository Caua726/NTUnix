/*
 * term.h — terminal hospedado pelo dispd.
 *
 * Cada janela WK_TERM tem um Terminal: um processo filho (busybox ash) cujo
 * stream VT e' lido por uma thread e parseado (vt.c) numa grade de celulas;
 * o compositor renderiza a grade no DIB da janela. Backends de hosting atras
 * de TerminalBackend: ConPTY (primario) e scrape (fallback, console oculto).
 */
#ifndef DISPD_TERM_H
#define DISPD_TERM_H

#include "../common/ntu.h"

struct Window;

/* atributos de celula */
#define ATTR_BOLD    0x01
#define ATTR_REVERSE 0x02

typedef struct Cell {
    unsigned char ch;    /* byte imprimivel (CP437/ASCII) */
    unsigned char fg;    /* indice 0..15 */
    unsigned char bg;    /* indice 0..15 */
    unsigned char attr;  /* ATTR_* */
} Cell;

/* estados do parser VT */
enum { VT_GROUND = 0, VT_ESC, VT_CSI, VT_OSC };

typedef struct Terminal {
    struct TerminalBackend *be;
    void  *impl;              /* privado do backend */

    int    cols, rows;
    Cell  *grid;              /* cols*rows, [row*cols + col] */
    int    cur_x, cur_y;
    int    cur_vis;
    volatile int dirty;       /* conteudo mudou desde o ultimo render */
    volatile int alive;       /* filho ainda vivo */
    volatile long rx;         /* diagnostico: bytes vindos do filho */
    DWORD  pid;               /* PID do processo filho (#41) */

    struct Window *win;       /* dono (usado pelo compositor, nao pelo vt) */
    CRITICAL_SECTION lock;    /* grade tocada pela thread leitora */

    char   title[256];        /* titulo via OSC 0/2 (escrito sob lock) */
    int    title_changed;     /* main thread consome e emite WINDOW-TITLE */

    /* estado do parser VT (vt.c) */
    int    vt_state;
    int    params[8];
    int    nparams;
    int    priv;              /* '?' em CSI */
    unsigned char cur_fg, cur_bg, cur_attr;
    char   osc[256];
    int    osc_len;

    HANDLE reader;            /* thread leitora do stream de saida */
} Terminal;

typedef struct TerminalBackend {
    const char *name;
    int  (*start) (Terminal *t, const char *cmdline, int cols, int rows);
    void (*input) (Terminal *t, const char *bytes, int n);
    void (*resize)(Terminal *t, int cols, int rows);
    void (*close) (Terminal *t);
} TerminalBackend;

/* fabrica: tenta ConPTY, cai no scrape. win pode ser NULL (selftest). */
Terminal *term_create(const char *cmdline, int cols, int rows, struct Window *win);
void      term_destroy(Terminal *t);
void      term_input(Terminal *t, const char *bytes, int n);
void      term_resize(Terminal *t, int cols, int rows);

/* backends (retornam 0 em sucesso) */
int  term_conpty_start(Terminal *t, const char *cmdline, int cols, int rows);
extern TerminalBackend term_conpty_backend;
extern TerminalBackend term_scrape_backend;

/* vt.c */
void vt_init(Terminal *t, int cols, int rows);
void vt_free(Terminal *t);
void vt_resize(Terminal *t, int cols, int rows);
void vt_feed(Terminal *t, const char *bytes, int n);  /* pega o lock internamente */
/* render: desenha a grade no memdc (chamado pelo compositor, main thread) */
void vt_render(Terminal *t, HDC memdc, HFONT font, int cellw, int cellh);

#endif
