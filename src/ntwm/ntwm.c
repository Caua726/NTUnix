/*
 * ntwm.c — window manager tiling do NTUnix (main).
 *
 * Cliente do dispd: conecta, faz HELLO, registra os grabs de teclado e entra
 * no loop de eventos. Cada evento atualiza o modelo e (re)declara o layout.
 * Trocavel em runtime: se sair, o initd sobe outro (ou voce troca o binario)
 * sem derrubar o dispd nem as sessoes.
 *
 * Binds (mod = Alt, estilo dwm):
 *   Alt+Enter  novo terminal        Alt+Shift+Enter  promove a master (zoom)
 *   Alt+j/k    foco prox/ant         Alt+h/l          master menor/maior
 *   Alt+i/d    +/- master            Alt+q            fecha o focado
 *   Alt+1..9   ve workspace          Alt+Shift+1..9   move focado p/ workspace
 *   Alt+Shift+e  sai (initd respawna / swap)
 */
#include "ntwm.h"
#include <stdlib.h>

unsigned g_mod = MOD_ALT;

static void cfg_kv(const char *sec, const char *key, const char *val, void *ud)
{
    (void)sec; (void)ud;
    /* tudo clampeado para nao gerar geometria absurda (#34) */
    if (!_stricmp(key, "nmaster")) {
        int v = atoi(val); g_nmaster = v < 0 ? 0 : (v > 16 ? 16 : v);
    } else if (!_stricmp(key, "mfact")) {
        double f = atof(val); if (f >= 0.05 && f <= 0.95) g_mfact = (float)f;
    } else if (!_stricmp(key, "gap")) {
        int v = atoi(val); g_gap = v < 0 ? 0 : (v > 200 ? 200 : v);
    } else if (!_stricmp(key, "border")) {
        int v = atoi(val); g_border = v < 0 ? 0 : (v > 32 ? 32 : v);
    } else if (!_stricmp(key, "mod")) {
        g_mod = !_stricmp(val, "win") ? MOD_WIN : MOD_ALT;
    }
}

void load_config(void)
{
    char p[MAX_PATH];
    ntu_path("/etc/ntwm/ntwm.conf", p, sizeof p);
    ntu_ini_parse(p, cfg_kv, NULL);   /* ausente -> mantem defaults */
}

static void grab(unsigned mods, unsigned vk)
{
    wm_send("%s %x %x", CMD_GRAB, mods, vk);
}

static void register_grabs(void)
{
    unsigned M = g_mod;
    grab(M, VK_RETURN);
    grab(M | MOD_SHIFT, VK_RETURN);
    grab(M, 'J'); grab(M, 'K');
    grab(M, 'H'); grab(M, 'L');
    grab(M, 'I'); grab(M, 'D');
    grab(M, 'Q');
    grab(M | MOD_SHIFT, 'E');
    grab(M | MOD_SHIFT, VK_SPACE);   /* alterna floating */
    for (int i = 0; i < 9; i++) {
        grab(M, (unsigned)('1' + i));
        grab(M | MOD_SHIFT, (unsigned)('1' + i));
    }
}

static void on_key(unsigned mods, unsigned vk)
{
    if (mods == g_mod) {
        switch (vk) {
        case VK_RETURN: wm_send("%s", CMD_SPAWN); break;
        case 'J': focusstack(+1); break;
        case 'K': focusstack(-1); break;
        case 'H': setmfact(-0.05f); break;
        case 'L': setmfact(+0.05f); break;
        case 'I': incnmaster(+1); break;
        case 'D': incnmaster(-1); break;
        case 'Q': killfocused(); break;
        default:
            if (vk >= '1' && vk <= '9')
                view((int)(vk - '1'));
            break;
        }
    } else if (mods == (unsigned)(g_mod | MOD_SHIFT)) {
        if (vk == VK_RETURN)
            zoom();
        else if (vk == 'E')
            exit(0);                 /* ntwm sai; initd respawna / swap */
        else if (vk == VK_SPACE)
            togglefloating();
        else if (vk >= '1' && vk <= '9')
            tagto((int)(vk - '1'));
    }
}

static void set_title(Client *c, const char *t)
{
    if (!c || !t) return;
    strncpy(c->title, t, sizeof c->title - 1);
    c->title[sizeof c->title - 1] = 0;
}

static void handle(char *line)
{
    /* copia o verbo antes do split destruir a linha */
    char *sp = strchr(line, ' ');
    char verb[24];
    int vl = sp ? (int)(sp - line) : (int)strlen(line);
    if (vl >= (int)sizeof verb) vl = (int)sizeof verb - 1;
    memcpy(verb, line, vl);
    verb[vl] = 0;

    /* titulo (ultimo campo) e' tomado verbatim conforme o verbo */
    int tail = -1;
    if (!strcmp(verb, EVT_WINDOW)) tail = 6;        /* WINDOW id kind pid ws float title */
    else if (!strcmp(verb, EVT_CREATED)) tail = 4;  /* WINDOW-CREATED id kind pid title */
    else if (!strcmp(verb, EVT_TITLE)) tail = 2;    /* WINDOW-TITLE id title */

    char *av[12];
    int n = ntuwm_split(line, av, 12, tail);
    if (n < 1)
        return;

    if (!strcmp(verb, EVT_WELCOME)) {
        if (n >= 3 && atoi(av[2]) != NTUWM_PROTO_VER)   /* valida versao (#65) */
            return;                                     /* dispd incompativel: nao opera */
        register_grabs();
    } else if (!strcmp(verb, EVT_OUTPUT) && n >= 6) {
        g_wx = atoi(av[2]); g_wy = atoi(av[3]);
        g_ww = atoi(av[4]); g_wh = atoi(av[5]);
    } else if (!strcmp(verb, EVT_CURWS) && n >= 2) {
        g_curws = atoi(av[1]);                       /* restart-survival (#11) */
    } else if (!strcmp(verb, EVT_WINDOW) && n >= 5) {
        int ws = atoi(av[4]);
        int fl = n >= 6 ? atoi(av[5]) : 0;           /* floating do snapshot (#33) */
        Client *c = cl_add((unsigned)strtoul(av[1], NULL, 10), ws, fl);
        if (n >= 7) set_title(c, av[6]);             /* snapshot: tila no SYNC */
    } else if (!strcmp(verb, EVT_CREATED) && n >= 2) {
        Client *c = cl_add((unsigned)strtoul(av[1], NULL, 10), g_curws, 0);
        if (n >= 5) set_title(c, av[4]);             /* #40 */
        send_frame();
    } else if (!strcmp(verb, EVT_DESTROYED) && n >= 2) {
        cl_remove((unsigned)strtoul(av[1], NULL, 10));
        send_frame();
    } else if (!strcmp(verb, EVT_TITLE) && n >= 3) {
        set_title(cl_find((unsigned)strtoul(av[1], NULL, 10)), av[2]);
    } else if (!strcmp(verb, EVT_FOCUSED) && n >= 2) {
        g_focused = cl_find((unsigned)strtoul(av[1], NULL, 10));   /* #9 */
    } else if (!strcmp(verb, EVT_KEY) && n >= 3) {
        on_key((unsigned)strtoul(av[1], NULL, 16),
               (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(verb, EVT_SYNC) || !strcmp(verb, EVT_RESYNC)) {
        send_frame();               /* layout inicial (SYNC) ou re-declaracao (RESYNC #71) */
    }
}

int main(void)
{
    load_config();
    if (wm_connect() != 0)
        return 1;
    wm_send("%s ntwm %d", CMD_HELLO, NTUWM_PROTO_VER);

    char buf[8192];
    for (;;) {
        int n = wm_read(buf, sizeof buf);
        if (n <= 0)
            break;                  /* dispd saiu */
        buf[n] = 0;
        for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
            ntu_trim(line);
            if (*line)
                handle(line);
        }
    }
    return 0;
}
