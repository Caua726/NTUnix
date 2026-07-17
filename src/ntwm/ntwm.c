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
    if (!_stricmp(key, "nmaster"))      g_nmaster = atoi(val);
    else if (!_stricmp(key, "mfact"))   g_mfact = (float)atof(val);
    else if (!_stricmp(key, "gap"))     g_gap = atoi(val);
    else if (!_stricmp(key, "border"))  g_border = atoi(val);
    else if (!_stricmp(key, "mod"))
        g_mod = !_stricmp(val, "win") ? MOD_WIN : MOD_ALT;
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

static void handle(char *line)
{
    /* copia o verbo antes do split destruir a linha */
    char *sp = strchr(line, ' ');
    char verb[24];
    int vl = sp ? (int)(sp - line) : (int)strlen(line);
    if (vl >= (int)sizeof verb) vl = (int)sizeof verb - 1;
    memcpy(verb, line, vl);
    verb[vl] = 0;

    char *av[10];
    int n = ntuwm_split(line, av, 10, -1);
    if (n < 1)
        return;

    if (!strcmp(verb, EVT_WELCOME)) {
        register_grabs();
    } else if (!strcmp(verb, EVT_OUTPUT) && n >= 6) {
        g_wx = atoi(av[2]); g_wy = atoi(av[3]);
        g_ww = atoi(av[4]); g_wh = atoi(av[5]);
    } else if (!strcmp(verb, EVT_WINDOW) && n >= 2) {
        cl_add((unsigned)strtoul(av[1], NULL, 10), g_curws);   /* snapshot: tila no SYNC */
    } else if (!strcmp(verb, EVT_CREATED) && n >= 2) {
        cl_add((unsigned)strtoul(av[1], NULL, 10), g_curws);
        send_frame();
    } else if (!strcmp(verb, EVT_DESTROYED) && n >= 2) {
        cl_remove((unsigned)strtoul(av[1], NULL, 10));
        send_frame();
    } else if (!strcmp(verb, EVT_TITLE) && n >= 3) {
        Client *c = cl_find((unsigned)strtoul(av[1], NULL, 10));
        if (c) { strncpy(c->title, av[2], sizeof c->title - 1);
                 c->title[sizeof c->title - 1] = 0; }
    } else if (!strcmp(verb, EVT_KEY) && n >= 3) {
        on_key((unsigned)strtoul(av[1], NULL, 16),
               (unsigned)strtoul(av[2], NULL, 16));
    } else if (!strcmp(verb, EVT_SYNC)) {
        send_frame();               /* layout inicial apos o snapshot */
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
