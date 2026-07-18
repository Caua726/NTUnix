/*
 * ntuwm.h — protocolo do display server NTUnix (dispd <-> ntwm).
 *
 * Header-only, compartilhado pelos dois processos (como ntu.h). Define os
 * verbos, os bits de modificador e helpers de parse. Nao entra em COMMON:
 * cada binario so faz #include (depois de <windows.h>, via ntu.h).
 *
 * Fio: linhas ASCII "VERBO arg1 arg2 ...". O ultimo campo textual (titulo,
 * cmdline) e' tomado verbatim ate o fim da linha, sem escaping. Mods sao
 * bitmask hex; VKs sao codigos Win32 (virtual-key) em hex.
 *
 * Transacao de layout: ntwm envia FRAME-BEGIN, uma serie de PLACE/FOCUS/...,
 * e FRAME-COMMIT; dispd aplica o quadro de forma atomica no COMMIT (sem as
 * corridas do X11). dispd e' dono de todo o estado de janela; um ntwm novo
 * que conecta recebe um snapshot (WINDOW... + SYNC) e re-declara o layout.
 */
#ifndef NTUWM_H
#define NTUWM_H

#define NTUWM_PROTO_VER 1

/* numero de workspaces do desktop — unico limite compartilhado (#4): o dispd
 * valida, a barra desenha e o ntwm mapeia Alt+1..9 exatamente esta faixa. */
#define NTUWM_WS 9

/* Modificadores: usamos os do windows.h (MOD_ALT=1, MOD_CONTROL=2,
 * MOD_SHIFT=4, MOD_WIN=8 — os mesmos do RegisterHotKey). So aliasamos CTRL. */
#ifndef MOD_CTRL
#define MOD_CTRL MOD_CONTROL
#endif

/* eventos dispd -> ntwm */
#define EVT_WELCOME    "WELCOME"      /* WELCOME dispd <ver> <root>            */
#define EVT_OUTPUT     "OUTPUT"       /* OUTPUT <oid> <x> <y> <w> <h>          */
#define EVT_CURWS      "CURWS"        /* CURWS <n>  (workspace atual, no snapshot) */
#define EVT_WINDOW     "WINDOW"       /* WINDOW <wid> <kind> <pid> <ws> <float> <title...>  (snapshot) */
#define EVT_CREATED    "WINDOW-CREATED" /* <wid> <kind> <pid> <title...>  (novo no ws atual) */
#define EVT_DESTROYED  "WINDOW-DESTROYED" /* <wid> */
#define EVT_TITLE      "WINDOW-TITLE"  /* <wid> <title...> */
#define EVT_FOCUSED    "FOCUSED"      /* <wid>  (foco mudou no dispd, ex.: mouse) */
#define EVT_KEY        "KEY"          /* KEY <mods> <vk>  (so combos com grab) */
#define EVT_SYNC       "SYNC"
#define EVT_RESYNC     "RESYNC"       /* dispd perdeu comandos -> WM re-declara o quadro */
#define EVT_ERR        "ERR"          /* ERR <motivo...> */
#define EVT_PING       "PING"         /* dispd -> ntwm: heartbeat (espera PONG) #62 */

/* comandos ntwm -> dispd */
#define CMD_HELLO      "HELLO"        /* HELLO ntwm <ver> */
#define CMD_FRAME_BEGIN  "FRAME-BEGIN"
#define CMD_PLACE      "PLACE"        /* PLACE <wid> <x> <y> <w> <h> <ws> <z> */
#define CMD_FOCUS      "FOCUS"        /* FOCUS <wid> */
#define CMD_WORKSPACE  "WORKSPACE"    /* WORKSPACE <n> */
#define CMD_SETWS      "WS"           /* WS <wid> <n>  (muda o ws da janela sem redimensionar) */
#define CMD_BORDER     "BORDER"       /* BORDER <wid> <px> <rrggbb> */
#define CMD_TITLEBAR   "TITLEBAR"     /* TITLEBAR <wid> <on|off> <text...> */
#define CMD_FRAME_COMMIT "FRAME-COMMIT"
#define CMD_SPAWN      "SPAWN-TERM"   /* SPAWN-TERM [cmdline...] */
#define CMD_CLOSE      "CLOSE"        /* CLOSE <wid> */
#define CMD_FLOAT      "FLOAT"        /* FLOAT <wid> <0|1>  (persiste no dispd p/ restart) */
#define CMD_GRAB       "GRAB"         /* GRAB <mods> <vk> */
#define CMD_UNGRAB     "UNGRAB"       /* UNGRAB <mods> <vk> */
#define CMD_QUIT       "QUIT"
#define CMD_PONG       "PONG"         /* ntwm -> dispd: resposta ao PING (liveness) #62 */

/*
 * Split reentrante em tokens separados por espaco/tab. Devolve a contagem.
 * Se 'tail_at' >= 0, ao atingir esse indice o resto da linha (verbatim, ja
 * sem espacos iniciais) vira o ultimo token — para titulos/cmdlines com
 * espacos. Passe tail_at < 0 para split simples. 'line' e' modificado.
 */
static inline int ntuwm_split(char *line, char *argv[], int max, int tail_at)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (tail_at >= 0 && n == tail_at) {
            argv[n++] = p;      /* resto verbatim ate o fim */
            break;
        }
        argv[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    return n;
}

#endif
