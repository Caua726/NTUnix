/*
 * present_gdi.c — backend de apresentacao via GDI BitBlt.
 *
 * Garantido no WinPE: WS_POPUP fullscreen + BitBlt do backbuffer na tela e'
 * exatamente como o proprio cmd do WinPE pinta. Entra no M0 e serve de
 * fallback quando o backend DXGI (M3) nao inicializa.
 */
#include "present.h"

typedef struct {
    HWND root;
    HDC  rootdc;
    int  w, h;
} GdiImpl;

static int gdi_init(PresentBackend *b, HWND root, int w, int h)
{
    GdiImpl *g = (GdiImpl *)b->impl;
    g->root = root;
    g->rootdc = GetDC(root);
    g->w = w;
    g->h = h;
    return g->rootdc ? 0 : -1;
}

static void gdi_present(PresentBackend *b, const Frame *f)
{
    GdiImpl *g = (GdiImpl *)b->impl;
    if (!g->rootdc)
        return;
    BitBlt(g->rootdc, 0, 0, f->w, f->h, f->memdc, 0, 0, SRCCOPY);
}

static int gdi_resize(PresentBackend *b, int w, int h)
{
    GdiImpl *g = (GdiImpl *)b->impl;
    g->w = w;
    g->h = h;
    return 0;
}

static void gdi_destroy(PresentBackend *b)
{
    GdiImpl *g = (GdiImpl *)b->impl;
    if (g->rootdc)
        ReleaseDC(g->root, g->rootdc);
    free(g);
    free(b);
}

PresentBackend *present_gdi_create(void)
{
    PresentBackend *b = (PresentBackend *)calloc(1, sizeof *b);
    GdiImpl *g = (GdiImpl *)calloc(1, sizeof *g);
    if (!b || !g) {
        free(b);
        free(g);
        return NULL;
    }
    b->name = "gdi";
    b->init = gdi_init;
    b->present = gdi_present;
    b->resize = gdi_resize;
    b->destroy = gdi_destroy;
    b->impl = g;
    return b;
}
