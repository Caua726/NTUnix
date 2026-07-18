/*
 * term.h — terminal hospedado pelo dispd.
 *
 * O emulador VT e' o libvterm (third_party/libvterm) — engine do vim/neovim,
 * robusto e completo (responde DSR/DA/CPR sozinho, alt-screen, scroll region,
 * mouse, cores). O caminho ConPTY alimenta bytes no libvterm (vt_feed) e o
 * compositor le a grade de celulas do libvterm (vt_render). O fallback scrape
 * (console oculto, WinPE sem ConPTY) escreve numa grade simples propria — por
 * isso `grid` continua existindo e o render tem dois caminhos.
 */
#ifndef DISPD_TERM_H
#define DISPD_TERM_H

#include "../common/ntu.h"

struct Window;

/* atributos de celula (usados pelo caminho scrape; o libvterm traz os seus) */
#define ATTR_BOLD      0x01
#define ATTR_REVERSE   0x02
#define ATTR_UNDERLINE 0x04

typedef struct Cell {
    unsigned int   ch;    /* codepoint Unicode (0/espaco = vazio) */
    COLORREF       fg;    /* cor resolvida (RGB) */
    COLORREF       bg;    /* cor resolvida (RGB) */
    unsigned short attr;  /* ATTR_* */
} Cell;

/* uma linha de scrollback (rolou pra fora do topo da tela) */
typedef struct SBLine {
    Cell *cells;
    int   len;
} SBLine;

typedef struct Terminal {
    struct TerminalBackend *be;
    void  *impl;              /* privado do backend */
    int    backend_is_pty;    /* 1=ConPTY: usa libvterm + aceita write-back */

    int    cols, rows;

    /* emulador libvterm (caminho ConPTY/VT); NULL no caminho scrape */
    void  *vt;                /* VTerm*        (opaco aqui) */
    void  *vts;               /* VTermScreen*  (opaco aqui) */
    int    cur_x, cur_y;      /* cursor (via callback do libvterm / scrape) */
    int    cur_vis;

    /* grade simples do fallback scrape (usada so quando vt == NULL) */
    Cell  *grid;

    /* scrollback (ring de linhas que rolaram do topo; so no caminho libvterm) */
    SBLine *sb;
    int    sb_cap, sb_count, sb_head;
    int    scroll_off;        /* linhas roladas p/ cima (0 = fundo/tempo real) */
    int    on_alt;            /* tela alternativa (vim/htop): sem scrollback */

    volatile int  dirty;      /* conteudo mudou desde o ultimo render */
    volatile long alive;      /* filho ainda vivo (Interlocked) */
    volatile long rx;         /* diagnostico: bytes vindos do filho (Interlocked) */
    DWORD  pid;               /* PID do processo filho */

    struct Window *win;       /* dono (usado pelo compositor) */
    CRITICAL_SECTION lock;    /* protege o modelo (libvterm/grade) e o cursor */

    char   title[256];        /* titulo via OSC (VTERM_PROP_TITLE / scrape) */
    int    title_len;         /* acumulacao dos fragmentos de titulo do libvterm */
    int    title_changed;     /* main thread consome e emite WINDOW-TITLE */

    /* saida do libvterm de volta ao pty (DSR/DA/mouse): montada no callback
     * sob o lock, enviada depois de liberar */
    /* audit #96: era 512 e descartava fragmentos inteiros no overflow (app
     * podia travar esperando resposta). 8K cobre um chunk cheio de queries;
     * fila 100% dinamica fica como follow-up. */
    char   reply[8192];
    int    reply_len;

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
/* encaminha um evento de mouse ao pty (via libvterm; no-op sem rastreio).
 * mods = MOD_* (ntuwm.h) do teclado no momento — audit #21 */
void      term_mouse(Terminal *t, int col, int row, int button, int press, int motion, unsigned mods);

/* backends (retornam 0 em sucesso) */
int  term_conpty_start(Terminal *t, const char *cmdline, int cols, int rows);
extern TerminalBackend term_conpty_backend;
extern TerminalBackend term_scrape_backend;
extern TerminalBackend term_pty_backend;    /* pty nativo (musl-nt), sem ConPTY */

/* vt.c (glue com o libvterm + render) */
int  vt_init(Terminal *t, int cols, int rows);   /* 0=ok, -1=sem memoria */
int  vt_use_libvterm(Terminal *t);               /* liga o libvterm (caminho ConPTY) */
void vt_free(Terminal *t);
void vt_resize(Terminal *t, int cols, int rows);
void vt_feed(Terminal *t, const char *bytes, int n);  /* pega o lock internamente */
void vt_render(Terminal *t, HDC memdc, HFONT font, int cellw, int cellh);
void vt_scroll(Terminal *t, int delta_lines);  /* +cima (historico) / -baixo */
void vt_scroll_reset(Terminal *t);             /* volta pro fundo (tempo real) */

/* paleta ANSI 0..15 -> RGB (compartilhada com o scrape) */
COLORREF vt_ansi_color(int idx);

/* log (definido em dispd.c) — disponivel para os backends de terminal */
void dispd_log(const char *fmt, ...);

#endif
