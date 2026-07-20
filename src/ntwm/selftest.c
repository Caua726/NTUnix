/* Fixtures deterministicas do modelo de politica. Rodam sem dispd. */
#include "ntwm.h"

static int leaves(DwindleNode *n)
{
    if (!n) return 0;
    if (n->client) return 1;
    return leaves(n->child[0]) + leaves(n->child[1]);
}

static int fail(int code, const char *message)
{
    char b[256];
    snprintf(b, sizeof b, "ntwm selftest %d: %s\n", code, message);
    OutputDebugStringA(b);
    return code;
}

int wm_selftest(void)
{
    wm_config_defaults(&g_wm.config);
    wm_state_init();
    g_wm.wx = 0; g_wm.wy = 0; g_wm.ww = 1920; g_wm.wh = 1080;

    Client *a = cl_add(1, 0, 0, 0, 0, "a");
    Client *b = cl_add(2, 0, 0, 0, 0, "b");
    Client *c = cl_add(3, 0, 0, 0, 0, "c");
    Workspace *ws = wm_workspace(0);
    if (!a || !b || !c || leaves(ws->dwindle_root) != 3)
        return fail(1, "insercao dwindle");
    wm_request_frame();
    if (a->rect.w < 1 || b->rect.w < 1 || c->rect.w < 1)
        return fail(2, "geometria dwindle");
    cl_remove(2);
    if (leaves(ws->dwindle_root) != 2 || !a->dwindle_leaf || !c->dwindle_leaf)
        return fail(3, "colapso do pai dwindle");

    /* Clique sem ultrapassar o threshold nao pode reordenar nem flutuar. */
    g_wm.waiting_serial = 0;
    wm_request_frame();
    g_wm.waiting_serial = 0;
    DwindleNode *root_before = ws->dwindle_root;
    int ax = a->rect.x + a->rect.w / 2, ay = a->rect.y + a->rect.h / 2;
    wm_pointer_event(a->id, ax, ay, 0, 1, MOD_ALT);
    wm_pointer_event(a->id, ax, ay, 0, 0, MOD_ALT);
    if (a->floating || ws->dwindle_root != root_before || leaves(ws->dwindle_root) != 2)
        return fail(4, "click virou drag/reinsercao");

    /* Resize RMB tiled altera a arvore, nunca o estado floating. */
    float ratio_before = ws->dwindle_root->ratio;
    Client *left = ws->dwindle_root->child[0]->client;
    wm_resize_pointer(left, 80, 0, WM_EDGE_RIGHT);
    if (left->floating || ws->dwindle_root->ratio <= ratio_before)
        return fail(5, "resize tiled dwindle");

    /* Move tiled acompanha como floating temporario e volta para a arvore. */
    g_wm.waiting_serial = 0;
    wm_request_frame();
    g_wm.waiting_serial = 0;
    ax = a->rect.x + a->rect.w / 2; ay = a->rect.y + a->rect.h / 2;
    int cx = c->rect.x + c->rect.w / 4, cy = c->rect.y + c->rect.h / 2;
    wm_pointer_event(a->id, ax, ay, 0, 1, MOD_ALT);
    wm_pointer_event(a->id, cx, cy, 0, 2, MOD_ALT);
    if (!a->floating || a->dwindle_leaf)
        return fail(6, "move tiled nao iniciou preview");
    wm_pointer_event(a->id, cx, cy, 0, 0, MOD_ALT);
    if (a->floating || !a->dwindle_leaf || leaves(ws->dwindle_root) != 2)
        return fail(7, "move tiled nao reinseriu");

    /* Floating resize pelo canto superior esquerdo ancora o canto oposto. */
    g_wm.waiting_serial = 0;
    wm_request_frame();
    g_wm.waiting_serial = 0;
    wm_client_set_floating(a, 1);
    WmRect initial = a->rect;
    wm_pointer_event(a->id, initial.x + 1, initial.y + 1, 2, 1, MOD_ALT);
    wm_pointer_event(a->id, initial.x + 21, initial.y + 16, 2, 2, MOD_ALT);
    if (a->float_rect.x != initial.x + 20 ||
        a->float_rect.y != initial.y + 15 ||
        a->float_rect.w != initial.w - 20 ||
        a->float_rect.h != initial.h - 15)
        return fail(8, "resize floating por top-left");
    wm_pointer_event(a->id, initial.x + 21, initial.y + 16, 2, 0, MOD_ALT);
    wm_reinsert(a, c->rect.x + c->rect.w / 2, c->rect.y + c->rect.h / 2);

    Client *e = cl_add(5, 0, 0, 0, 0, "e");
    if (!e)
        return fail(9, "cliente para master");
    g_wm.waiting_serial = 0;
    wm_set_layout(LAYOUT_MASTER);
    g_wm.waiting_serial = 0;
    wm_request_frame();
    if (e->rect.w == a->rect.w && e->rect.x == a->rect.x)
        return fail(10, "master nao separou regioes");

    Client *s1 = ws->clients ? ws->clients->ws_next : NULL;
    Client *s2 = s1 ? s1->ws_next : NULL;
    if (!s1 || !s2)
        return fail(11, "pilha master incompleta");
    float s1w = s1->tile_weight, s2w = s2->tile_weight;
    g_wm.waiting_serial = 0;
    wm_resize_pointer(s1, 0, 40, WM_EDGE_BOTTOM);
    if (s1->tile_weight <= s1w || s2->tile_weight >= s2w)
        return fail(12, "resize individual da pilha master");
    ws->orientation = MASTER_RIGHT;
    float mfact = ws->mfact;
    g_wm.waiting_serial = 0;
    wm_resize_pointer(ws->clients, 40, 0, WM_EDGE_RIGHT);
    if (ws->mfact >= mfact)
        return fail(13, "sinal do mfact master right");

    g_wm.config.nrules = 1;
    WmRule *r = &g_wm.config.rules[0];
    ZeroMemory(r, sizeof *r);
    r->match_kind = 1;
    r->match_workspace = -1;
    strcpy(r->title_glob, "*float*");
    r->effects = RULE_FLOATING | RULE_WORKSPACE | RULE_OPACITY;
    r->floating = 1; r->workspace = 2; r->opacity = 200;
    Client *d = cl_add(4, 1, 0, 0, 0, "please-float");
    if (!d || !d->floating || d->ws != 2 || d->style.opacity != 200)
        return fail(14, "ordem/efeitos de regra");

    wm_view(2);
    if (g_wm.cur_ws != 2 || wm_workspace(0)->layout != LAYOUT_MASTER ||
        wm_workspace(2)->focused != d)
        return fail(15, "estado por workspace");
    return 0;
}
