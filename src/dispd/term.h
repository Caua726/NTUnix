/*
 * term.h — terminal hospedado pelo dispd.
 *
 * Cada janela WK_TERM tem um Terminal: um processo filho (busybox ash) cujo
 * stream VT e' lido por uma thread e parseado (vt.c) numa grade de celulas;
 * o compositor renderiza a grade no DIB da janela. Backends de hosting atras
 * de TerminalBackend: ConPTY (primario) e scrape (fallback, console oculto).
 *
 * O parser (vt.c) e' uma maquina de estados VT completa (modelada no st/xterm):
 * CSI amplo, tela alternada, regiao de rolagem, save/restore de cursor, e
 * RESPONDE a queries (DSR/CPR/DA) escrevendo de volta no pty — sem isso o
 * busybox lineedit trava esperando a posicao do cursor.
 */
#ifndef DISPD_TERM_H
#define DISPD_TERM_H

#include "../common/ntu.h"

struct Window;

/* atributos de celula */
#define ATTR_BOLD      0x01
#define ATTR_REVERSE   0x02
#define ATTR_UNDERLINE 0x04
#define ATTR_ITALIC    0x08
#define ATTR_DIM       0x10
#define ATTR_BLINK     0x20
#define ATTR_INVIS     0x40
#define ATTR_WDUMMY    0x80   /* metade direita de um caractere largo */

typedef struct Cell {
    unsigned int   ch;    /* codepoint Unicode (0/espaco = vazio) */
    COLORREF       fg;    /* cor resolvida (RGB) — suporta truecolor */
    COLORREF       bg;    /* cor resolvida (RGB) */
    unsigned short attr;  /* ATTR_* */
} Cell;

/* estados do parser VT */
enum { VT_GROUND = 0, VT_ESC, VT_ESC_INT, VT_CSI, VT_CSI_INT, VT_OSC, VT_STR };

#define VT_MAXPARAMS 16
#define VT_REPLYCAP  64
#define VT_OSCCAP    512

typedef struct Terminal {
    struct TerminalBackend *be;
    void  *impl;              /* privado do backend */
    int    backend_is_pty;    /* 1=ConPTY: aceita respostas a queries (write-back) */

    int    cols, rows;
    Cell  *grid;              /* tela ativa: cols*rows, [row*cols + col] */
    Cell  *grid_main;         /* buffer da tela primaria */
    Cell  *grid_alt;          /* buffer da tela alternada */
    int    on_alt;            /* 1 = usando a tela alternada */

    int    cur_x, cur_y;
    int    cur_vis;
    int    wrap_pending;      /* autowrap adiado (escreveu na ultima coluna) */
    int    scroll_top, scroll_bot;   /* regiao de rolagem (0-based, inclusivo) */

    /* modos */
    int    mode_autowrap;     /* DECAWM (default on) */
    int    mode_origin;       /* DECOM: cursor relativo a regiao de rolagem */
    int    mode_appcursor;    /* DECCKM: setas em ESC O_ */
    int    mode_bracketed;    /* ?2004: bracketed paste */
    int    mode_mouse;        /* bitmask: modos de rastreio de mouse (?1000/1002/1003) */
    int    mode_mouse_sgr;    /* ?1006: encode SGR do mouse */

    volatile int dirty;       /* conteudo mudou desde o ultimo render */
    volatile long alive;      /* filho ainda vivo (Interlocked) */
    volatile long rx;         /* diagnostico: bytes vindos do filho (Interlocked) */
    DWORD  pid;               /* PID do processo filho */

    struct Window *win;       /* dono (usado pelo compositor, nao pelo vt) */
    CRITICAL_SECTION lock;    /* grade tocada pela thread leitora */

    char   title[256];        /* titulo via OSC 0/2 (escrito sob lock) */
    int    title_changed;     /* main thread consome e emite WINDOW-TITLE */

    /* estado do parser VT (vt.c) */
    int    vt_state;
    int    params[VT_MAXPARAMS];
    int    nparams;
    int    param_ovf;         /* algum parametro estourou -> descarta a sequencia */
    char   priv;              /* prefixo privado de CSI: '?','>','!' ou 0 */
    char   inter;             /* byte intermediario (0x20-0x2f) ou 0 */
    char   str_kind;          /* OSC/DCS/PM/APC em andamento */
    COLORREF cur_fg, cur_bg;
    unsigned short cur_attr;
    int    sv_x, sv_y;        /* cursor salvo (DECSC) */
    COLORREF sv_fg, sv_bg;
    unsigned short sv_attr;
    unsigned utf8_cp;         /* acumulador de decodificacao UTF-8 */
    int    utf8_left;         /* bytes de continuacao restantes */
    char   osc[VT_OSCCAP];
    int    osc_len;
    char  *tabs;              /* cols: 1 onde ha tab stop */

    /* resposta a queries (DSR/DA): montada sob lock, enviada apos liberar */
    char   reply[VT_REPLYCAP];
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

/* backends (retornam 0 em sucesso) */
int  term_conpty_start(Terminal *t, const char *cmdline, int cols, int rows);
extern TerminalBackend term_conpty_backend;
extern TerminalBackend term_scrape_backend;

/* vt.c */
int  vt_init(Terminal *t, int cols, int rows);   /* 0=ok, -1=sem memoria (#105) */
void vt_free(Terminal *t);
void vt_resize(Terminal *t, int cols, int rows);
void vt_feed(Terminal *t, const char *bytes, int n);  /* pega o lock internamente */
/* render: desenha a grade no memdc (chamado pelo compositor, main thread) */
void vt_render(Terminal *t, HDC memdc, HFONT font, int cellw, int cellh);

/* paleta ANSI 0..15 -> RGB (compartilhada com o scrape) */
COLORREF vt_ansi_color(int idx);

#endif
