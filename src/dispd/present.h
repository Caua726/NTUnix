/*
 * present.h — costura de apresentacao do dispd.
 *
 * O compositor compoe o quadro num backbuffer DIB proprio (dupla exposicao:
 * memdc para GDI, bgra cru para upload DXGI) e chama backend->present(frame).
 * Backends: GDI-BitBlt (M0, sempre funciona no WinPE) e DXGI flip-model
 * (M3, mesma costura). Escolha por env DISPD_BACKEND=gdi|dxgi (default gdi).
 */
#ifndef DISPD_PRESENT_H
#define DISPD_PRESENT_H

#include "../common/ntu.h"

typedef struct Frame {
    HDC     memdc;   /* DC com o DIB composto selecionado (caminho GDI) */
    HBITMAP dib;
    void   *bgra;    /* bits do DIB, top-down 32bpp BGRA (caminho DXGI) */
    int     w, h;
    int     stride;  /* bytes por linha (= w*4) */
} Frame;

typedef struct PresentBackend {
    const char *name;
    int  (*init)   (struct PresentBackend *b, HWND root, int w, int h);
    void (*present)(struct PresentBackend *b, const Frame *f);
    int  (*resize) (struct PresentBackend *b, int w, int h);
    void (*destroy)(struct PresentBackend *b);
    void *impl;
} PresentBackend;

PresentBackend *present_gdi_create(void);
PresentBackend *present_dxgi_create(void);   /* M3: flip-model; cai no gdi se falhar */

#endif
