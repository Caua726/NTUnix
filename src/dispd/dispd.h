/*
 * dispd.h — estado interno do display server NTUnix.
 *
 * dispd e' dono de UMA janela real (a raiz fullscreen); cada "janela" do
 * desktop e' uma superficie logica com DIB proprio (nunca lemos a superficie
 * de ninguem — evita o problema de redirection surface). O window manager
 * (ntwm) e' um processo separado que so declara layout via NTU_PIPE_DISPD.
 */
#ifndef DISPD_H
#define DISPD_H

#include "../common/ntu.h"
#include "present.h"
#include "term.h"

typedef enum { WK_TERM = 0, WK_APP = 1 } WinKind;

typedef struct Window {
    unsigned  id;
    WinKind   kind;
    char      title[256];
    DWORD     pid;

    RECT      rect;        /* retangulo completo na raiz (inclui borda) */
    int       ws;          /* workspace */
    int       z;           /* empilhamento dentro do ws */
    int       visible;
    int       focused;
    int       border_px;
    COLORREF  border_rgb;

    /* backing store do conteudo (area cliente = rect menos borda) */
    HDC       memdc;
    HBITMAP   dib;
    void     *bits;
    int       cw, ch;      /* tamanho em pixels da area cliente */
    int       dirty;

    Terminal *term;        /* kind==WK_TERM */
    HANDLE    section;     /* kind==WK_APP: section compartilhada com o app */
    struct Window *next;
} Window;

typedef struct Server {
    HWND      root;
    int       scr_w, scr_h;

    Window   *windows;
    unsigned  next_id;
    Window   *focused;
    int       cur_ws;

    PresentBackend *present;

    /* backbuffer composto */
    HDC       cdc;
    HBITMAP   cdib;
    void     *cbits;
    Frame     frame;

    /* fonte monoespacada + metrica de celula */
    HFONT     font;
    int       cellw, cellh;

    int       bar_h;       /* altura da barra de status no topo */
    int       title_h;     /* altura da barra de titulo por janela (0=off) */
    int       dirty;       /* precisa recompor+apresentar neste tick */
    int       ffm;         /* focus-follows-mouse (DISPD_FFM=1) */
    long      keys_seen;   /* diagnostico: teclas capturadas pelo hook */
    int       selftest;    /* DISPD_SELFTEST=1: so retangulos, sem term/ntwm */
    int       running;
} Server;

extern Server g_srv;

#define DISP_BG        RGB(24, 24, 28)
#define BORDER_NORMAL  RGB(64, 64, 72)
#define BORDER_FOCUS   RGB(122, 162, 247)

/* compositor.c */
void     compositor_init(void);
Window  *win_create(WinKind kind, int cw, int ch);
Window  *win_create_shared(int cw, int ch, HANDLE section); /* WK_APP: DIB sobre section */
void     win_destroy(Window *w);
Window  *win_find(unsigned id);
void     win_set_client_size(Window *w, int cw, int ch);
void     win_focus(Window *w);
void     compose_and_present(void);
Window  *win_at_point(int x, int y);   /* topo visivel no ws atual */

/* input.c — teclado: hook LL global + WM_KEYDOWN (foco forcado), redundantes */
void     input_install_hook(void);
int      input_key(unsigned vk, unsigned scan);   /* retorna 1 se tratou */

/* wmproto.c */
void     wmproto_start(void);                 /* sobe o servidor de pipe (thread) */
void     wmproto_drain(void);                 /* main thread: aplica comandos na fila */
int      wmproto_connected(void);
int      wmproto_grabbed(unsigned mods, unsigned vk);
void     wmproto_ev_created(Window *w);
void     wmproto_ev_destroyed(unsigned id);
void     wmproto_ev_title(Window *w);
void     wmproto_ev_key(unsigned mods, unsigned vk);

/* appsrv.c — fronteira apps<->dispd (surface compartilhada via section) */
void     appsrv_start(void);   /* sobe o servidor multi-cliente (thread) */
void     appsrv_drain(void);   /* main thread: aplica pedidos dos apps */

/* dispd.c */
Window  *spawn_terminal(const char *cmdline);  /* cria janela WK_TERM + pty */
void     dispd_log(const char *fmt, ...);

#endif
