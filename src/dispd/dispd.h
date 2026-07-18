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
    int       floating;    /* estado floating (declarado pelo WM; persiste p/ restart #33) */
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

    Terminal *term;        /* kind==WK_TERM: aba ATIVA (= tabs[active_tab]) */
    Terminal *tabs[16];    /* abas (cada uma = uma sessao tty/cmd/powershell) */
    int       ntabs;
    int       active_tab;

    HANDLE    section;     /* kind==WK_APP: section compartilhada com o app */
    struct Window *next;
} Window;
#define MAX_TABS 16

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
    int       in_frame;    /* dentro de FRAME-BEGIN..COMMIT (nao apresenta) */
    int       debug;       /* DISPD_DEBUG=1: diagnosticos na tela/log */
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
int      compositor_init(void);   /* 0=ok, -1=backbuffer falhou (#95) */
Window  *win_create(WinKind kind, int cw, int ch);
Window  *win_create_shared(int cw, int ch, HANDLE section); /* WK_APP: DIB sobre section */
void     win_destroy(Window *w);
Window  *win_find(unsigned id);
void     win_set_client_size(Window *w, int cw, int ch);
void     win_focus(Window *w);
void     compose_and_present(void);
Window  *win_at_point(int x, int y);   /* topo visivel no ws atual */

/* input.c — teclado: hook LL global (enfileira) + WM_KEYDOWN fallback */
void     input_install_hook(void);
void     input_process_keys(void);   /* main thread: drena e roteia (frame_tick) */
int      input_hook_active(void);
void     input_key(unsigned vk, unsigned scan);     /* fallback WM_KEYDOWN */
void     input_keyup(unsigned vk, unsigned scan);   /* fallback WM_KEYUP (#13) */
void     input_mouse(int x, int y, int button, int press, int motion);  /* #8/#9 */

/* wmproto.c */
void     wmproto_start(void);                 /* sobe o servidor de pipe (thread) */
void     wmproto_drain(void);                 /* main thread: aplica comandos na fila */
void     wmproto_abort_frame(void);           /* descarta um FRAME travado (#30) */
int      wmproto_connected(void);
int      wmproto_grabbed(unsigned mods, unsigned vk);
void     wmproto_ev_created(Window *w);
void     wmproto_ev_destroyed(unsigned id);
void     wmproto_ev_title(Window *w);
void     wmproto_ev_focused(Window *w);   /* foco mudou no dispd (ex.: mouse) */
void     wmproto_ev_key(unsigned mods, unsigned vk);

/* appsrv.c — fronteira apps<->dispd (surface compartilhada via section) */
void     appsrv_start(void);   /* sobe o servidor multi-cliente (thread) */
void     appsrv_drain(void);   /* main thread: aplica pedidos dos apps */
void     appsrv_close_wid(unsigned id);            /* fecha a conexao do app */
void     appsrv_input_key(unsigned id, unsigned mods, unsigned vk, unsigned ch, int down);
void     appsrv_input_mouse(unsigned id, int x, int y, int buttons);

/* dispd.c */
Window  *spawn_terminal(const char *cmdline);  /* cria janela WK_TERM + 1a aba */
void     dispd_close_window(Window *w);        /* fecha janela + conexao do app (#54) */
void     dispd_log(const char *fmt, ...);

/* abas do terminal (dispd.c) — internas ao dispd; o WM nao sabe delas */
int      win_tab_add(Window *w, const char *cmdline);  /* nova aba, vira ativa */
void     win_tab_close(Window *w, int idx);            /* fecha; ultima -> fecha janela */
void     win_tab_switch(Window *w, int idx);           /* ativa a aba idx */

#endif
