/*
 * ntwm.h — estado do window manager (cliente do dispd).
 *
 * ntwm e' um processo separado e trocavel: so decide layout e fala o
 * protocolo (NTU_PIPE_DISPD). Single-thread sincrono — nunca le e escreve ao
 * mesmo tempo, entao dispensa overlapped/locks. Modelo portado do dwm.
 */
#ifndef NTWM_H
#define NTWM_H

#include "../common/ntu.h"
#include "../common/ntuwm.h"

typedef struct Client {
    unsigned id;
    int      ws;
    int      floating;
    char     title[128];
    struct Client *next;
} Client;

extern Client *g_clients;    /* cabeca = master (dwm-style) */
extern Client *g_focused;
extern int     g_curws;
extern int     g_nmaster;
extern float   g_mfact;
extern int     g_wx, g_wy, g_ww, g_wh;   /* area util (de OUTPUT) */
extern int     g_gap;        /* espaco entre janelas / borda externa */
extern int     g_border;     /* espessura da borda (px) */
extern unsigned g_mod;       /* modificador ($mod): MOD_ALT ou MOD_WIN */
extern HANDLE  g_pipe;

/* proto.c */
int  wm_connect(void);
void wm_send(const char *fmt, ...);
int  wm_read(char *buf, int cap);

/* layout.c */
Client *cl_add(unsigned id, int ws, int floating);
void    cl_remove(unsigned id);
Client *cl_find(unsigned id);
void    send_frame(void);
void    focusstack(int dir);
void    setmfact(float d);
void    incnmaster(int d);
void    view(int ws);
void    tagto(int ws);
void    zoom(void);
void    killfocused(void);
void    togglefloating(void);

/* ntwm.c (load_config + cfg_kv) — audit #122: nao existe config.c separado */
void    load_config(void);

#endif
