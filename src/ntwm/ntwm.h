/*
 * ntwm.h - politica do desktop NTUnix.
 *
 * O estado e deliberadamente separado por workspace. O dispd recebe apenas
 * snapshots logicos serializados; animacao e apresentacao ficam do outro lado
 * do protocolo.
 */
#ifndef NTWM_H
#define NTWM_H

#include "../common/ntu.h"
#include "../common/ntuwm.h"

#define WM_MAX_BINDS 128
#define WM_MAX_RULES 64
#define WM_MAX_ACTION 32
#define WM_MAX_ARG 256

typedef struct WmRect {
    int x, y, w, h;
} WmRect;

typedef enum {
    LAYOUT_DWINDLE = 0,
    LAYOUT_MASTER = 1
} LayoutKind;

typedef enum {
    MASTER_LEFT = 0,
    MASTER_RIGHT,
    MASTER_TOP,
    MASTER_BOTTOM,
    MASTER_CENTER
} MasterOrientation;

typedef enum {
    DIR_LEFT = 0,
    DIR_RIGHT,
    DIR_UP,
    DIR_DOWN
} WmDirection;

enum {
    WM_EDGE_NONE   = 0,
    WM_EDGE_TOP    = 1u << 0,
    WM_EDGE_BOTTOM = 1u << 1,
    WM_EDGE_LEFT   = 1u << 2,
    WM_EDGE_RIGHT  = 1u << 3
};

typedef struct Client Client;
typedef struct DwindleNode DwindleNode;
typedef struct Workspace Workspace;

typedef struct LayoutOps {
    const char *name;
    void    (*insert)(Workspace *ws, Client *c, Client *focused);
    void    (*remove)(Workspace *ws, Client *c);
    void    (*arrange)(Workspace *ws, WmRect area);
    Client *(*focus)(Workspace *ws, Client *from, WmDirection dir);
    void    (*move)(Workspace *ws, Client *from, Client *to);
    void    (*resize)(Workspace *ws, Client *c, WmDirection dir, float amount);
    int     (*message)(Workspace *ws, const char *message);
} LayoutOps;

struct DwindleNode {
    DwindleNode *parent;
    DwindleNode *child[2];
    Client       *client;       /* folha: client != NULL */
    float         ratio;        /* fracao do child[0], [0.10,0.90] */
    int           vertical;     /* 1 = esquerda/direita; 0 = cima/baixo */
    WmRect        rect;         /* regiao logica do no, usada em resize manual */
};

struct Workspace {
    int               index;
    Client           *clients;
    Client           *focused;
    LayoutKind        layout;
    DwindleNode      *dwindle_root;
    int               nmaster;
    float             mfact;
    MasterOrientation orientation;
    int               insert_master;
    int               gap_inner;
    int               gap_outer;
    char              name[32];
    const LayoutOps  *ops;
};

typedef struct WmStyle {
    int      border;
    unsigned border_rgb;
    int      opacity;       /* 0..255 */
    int      shadow;
    int      radius;
    int      animate;
    int      titlebar;
} WmStyle;

struct Client {
    unsigned      id;
    int           kind;
    DWORD         pid;
    int           ws;
    int           floating;
    int           fullscreen;
    int           maximized;
    int           urgent;
    int           managed;
    WmRect        rect;       /* ultimo destino logico */
    WmRect        float_rect;
    int           has_float_rect;
    float         tile_weight;  /* tamanho relativo dentro da coluna/linha master */
    char          title[256];
    char          exe[MAX_PATH];
    WmStyle       style;
    DwindleNode  *dwindle_leaf;
    Client       *next;       /* lista global */
    Client       *ws_next;    /* ordem local do workspace */
};

typedef struct WmBind {
    unsigned mods;
    unsigned vk;
    char     action[WM_MAX_ACTION];
    char     arg[WM_MAX_ARG];
} WmBind;

enum {
    RULE_FLOATING  = 1u << 0,
    RULE_WORKSPACE = 1u << 1,
    RULE_BORDER    = 1u << 2,
    RULE_OPACITY   = 1u << 3,
    RULE_SHADOW    = 1u << 4,
    RULE_ANIMATE   = 1u << 5,
    RULE_TITLEBAR  = 1u << 6
};

typedef struct WmRule {
    char     name[64];
    int      match_kind;       /* -1 = qualquer */
    int      match_workspace;  /* -1 = qualquer */
    char     title_glob[128];
    char     exe_glob[128];
    unsigned effects;
    int      floating;
    int      workspace;
    int      border;
    int      opacity;
    int      shadow;
    int      animate;
    int      titlebar;
} WmRule;

typedef struct WmConfig {
    unsigned          mod;
    LayoutKind        default_layout;
    int               gap_inner;
    int               gap_outer;
    WmStyle           style;
    int               animations;
    int               move_ms;
    int               open_ms;
    int               workspace_ms;
    int               focus_ms;
    int               dwindle_default_ratio; /* percent */
    int               drag_threshold;        /* pixels antes de iniciar move/resize */
    int               nmaster;
    float             mfact;
    MasterOrientation master_orientation;
    int               insert_master;
    int               bar_enabled;
    int               bar_height;
    WmBind            binds[WM_MAX_BINDS];
    int               nbinds;
    WmRule            rules[WM_MAX_RULES];
    int               nrules;
    struct {
        int set_layout, set_nmaster, set_mfact, set_orientation;
        LayoutKind layout;
        int nmaster;
        float mfact;
        MasterOrientation orientation;
        char name[32];
    } workspace[NTUWM_WS];
} WmConfig;

typedef struct WmState {
    Client    *clients;
    Workspace  workspaces[NTUWM_WS];
    int        cur_ws;
    int        wx, wy, ww, wh;
    WmConfig   config;
    unsigned   next_serial;
    unsigned   waiting_serial;
    int        frame_pending;
    int        running;
} WmState;

extern WmState g_wm;
extern HANDLE  g_pipe;

/* proto.c */
int  wm_connect(void);
void wm_send(const char *fmt, ...);
int  wm_read(char *buf, int cap); /* >0 mensagem, 0 idle, -1 desconectou */

/* layout.c */
void      wm_state_init(void);
Workspace *wm_workspace(int index);
Client    *cl_add(unsigned id, int kind, DWORD pid, int ws, unsigned flags,
                  const char *title);
void       cl_remove(unsigned id);
Client    *cl_find(unsigned id);
void       cl_set_title(Client *c, const char *title);
void       wm_client_set_workspace(Client *c, int ws);
void       wm_client_set_floating(Client *c, int floating);
void       wm_workspace_set_layout(Workspace *ws, LayoutKind layout);
void       wm_request_frame(void);
void       wm_frame_applied(unsigned serial);
void       wm_send_snapshot(void);
void       wm_focus_direction(WmDirection dir);
void       wm_move_direction(WmDirection dir);
void       wm_resize_direction(WmDirection dir, float amount);
void       wm_view(int ws);
void       wm_move_to_workspace(int ws);
void       wm_toggle_floating(void);
void       wm_toggle_fullscreen(void);
void       wm_toggle_maximized(void);
void       wm_set_layout(LayoutKind layout);
int        wm_layout_message(const char *message);
void       wm_close_focused(void);
void       wm_move_floating(Client *c, int x, int y, int w, int h);
void       wm_reinsert(Client *c, int x, int y);
void       wm_resize_pointer(Client *c, int dx, int dy, unsigned edges);

/* config.c */
void wm_config_defaults(WmConfig *cfg);
int  wm_config_load(WmConfig *out, char *error, size_t error_cap);
int  wm_reload_config(int initial);
void wm_apply_rules(Client *c);
int  wm_parse_mods(const char *s, unsigned *mods);
int  wm_parse_key(const char *s, unsigned *vk);

/* actions.c */
int  wm_dispatch(const char *action, const char *arg, char *reply, size_t reply_cap);
void wm_handle_key(unsigned mods, unsigned vk);
void wm_register_grabs(void);

/* ipc.c */
void wm_ipc_start(void);
void wm_ipc_stop(void);
void wm_ipc_drain(void);
void wm_ipc_event(const char *event, const char *data);
void wm_ipc_snapshot(void);

/* ntwm.c */
void wm_report(const char *fmt, ...);
int  wm_selftest(void);
void wm_pointer_event(unsigned id, int x, int y, int button, int state,
                      unsigned mods);

#endif
