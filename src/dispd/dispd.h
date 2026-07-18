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

typedef enum { WK_TERM = 0, WK_APP = 1, WK_FOREIGN = 2 } WinKind;

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
    HWND      hwnd;        /* kind==WK_FOREIGN: a janela do Windows gerenciada */
    LONG_PTR  orig_style;  /* estilo original (p/ restaurar ao soltar) */
    RECT      fg_target;   /* kind==WK_FOREIGN: onde a janela DEVE ficar (re-snap) */
    ULONGLONG anim_ms;     /* inicio da animacao de aparecer (0 = nenhuma/pronta) */
    ULONGLONG focus_ms;    /* inicio da transicao de foco da borda (#35) */
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
    int       dirty;       /* recompoe A TELA TODA neste tick (mudanca estrutural) */
    int       bar_dirty;   /* so a barra de status mudou (relogio/titulo) */
    int       in_frame;    /* dentro de FRAME-BEGIN..COMMIT (nao apresenta) */
    int       debug;       /* DISPD_DEBUG=1: diagnosticos na tela/log */
    int       ffm;         /* focus-follows-mouse (DISPD_FFM=1) */
    long      keys_seen;   /* diagnostico: teclas capturadas pelo hook */
    int       selftest;    /* DISPD_SELFTEST=1: so retangulos, sem term/ntwm */
    int       running;

    char      toast[192];  /* #2: mensagem transitoria (erro/feedback) no canto */
    ULONGLONG toast_ms;    /* inicio do toast (0 = nenhum) */
} Server;

extern Server g_srv;

#define DISP_BG        RGB(24, 24, 28)
#define BORDER_NORMAL  RGB(64, 64, 72)
#define BORDER_FOCUS   RGB(122, 162, 247)

/* compositor.c */
int      compositor_init(void);   /* 0=ok, -1=backbuffer falhou (#95) */
int      compositor_resize(int w, int h);   /* #85: recria backbuffer (WM_DISPLAYCHANGE) */
Window  *win_create(WinKind kind, int cw, int ch);
Window  *win_create_shared(int cw, int ch, HANDLE section); /* WK_APP: DIB sobre section */
void     win_destroy(Window *w);
Window  *win_find(unsigned id);
void     win_set_client_size(Window *w, int cw, int ch);
void     win_focus(Window *w);
void     compose_and_present(void);
int      compositor_animating(void);   /* #35: ha janela animando -> manter frames */
int      bar_ws_at(int x, int y);      /* #3: workspace clicado na barra (-1 = nenhum) */
Window  *win_at_point(int x, int y);   /* topo visivel no ws atual */

/* input.c — teclado: hook LL global (enfileira) + WM_KEYDOWN fallback */
void     input_install_hook(void);
void     input_hook_refresh(void);   /* #13: reinstala o hook (auto-cura) */
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
void     wmproto_ev_output(void);   /* #85: tela mudou -> WM re-tila */
void     wmproto_ev_wsreq(int ws);  /* #3: usuario clicou um workspace na barra */
void     wmproto_ev_moved(unsigned id, int x, int y, int w, int h);  /* #5: arrasto */

/* appsrv.c — fronteira apps<->dispd (surface compartilhada via section) */
void     appsrv_start(void);   /* sobe o servidor multi-cliente (thread) */
void     appsrv_drain(void);   /* main thread: aplica pedidos dos apps */
void     appsrv_close_wid(unsigned id);            /* fecha a conexao do app */
void     appsrv_input_key(unsigned id, unsigned mods, unsigned vk, unsigned ch, int down);
void     appsrv_input_mouse(unsigned id, int x, int y, int buttons);

/* foreign.c — gerencia janelas nativas do Windows (taskmgr, notepad, browser…)
 * como WK_FOREIGN: descobre via WinEventHook, tila via SetWindowPos (modelo
 * komorebi/GlazeWM). Rodam em win32k; a gente e' o WM, nao o compositor delas. */
void     foreign_init(void);          /* instala hooks + varre janelas abertas */
void     foreign_place(Window *w, int x, int y, int cw, int ch);  /* SetWindowPos */
void     foreign_focus(Window *w);    /* SetForegroundWindow */
void     foreign_release(Window *w);  /* restaura o estilo (ao deixar de gerenciar) */

/* dispd.c */
Window  *spawn_terminal(const char *cmdline);  /* cria janela WK_TERM + 1a aba */
Window  *win_create_foreign(HWND hwnd);        /* WK_FOREIGN (sem DIB) */
void     dispd_close_window(Window *w);        /* fecha janela + conexao do app (#54) */
void     dispd_log(const char *fmt, ...);
void     dispd_toast(const char *fmt, ...);    /* #2: feedback visual transitorio */

/* abas do terminal (dispd.c) — internas ao dispd; o WM nao sabe delas */
int      win_tab_add(Window *w, const char *cmdline);  /* nova aba, vira ativa */
void     win_tab_close(Window *w, int idx);            /* fecha; ultima -> fecha janela */
void     win_tab_switch(Window *w, int idx);           /* ativa a aba idx */

#endif
