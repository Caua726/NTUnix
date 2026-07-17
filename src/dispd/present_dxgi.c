/*
 * present_dxgi.c — backend de apresentacao via DXGI flip-model (M3).
 *
 * O alvo: swap chain flip-model borderless cobrindo o monitor (Independent
 * Flip) — o jeito publico/estavel de tomar o scanout (a MS desenhou o flip
 * model pra bypassar o proprio DWM). Aqui subimos o frame BGRA composto pelo
 * dispd numa staging texture e CopyResource no backbuffer — sem shaders.
 *
 * Runtime-load de D3D11CreateDevice (d3d11.dll) via GetProcAddress: se faltar
 * (WinPE fino / sem GPU), init() falha e o dispd cai no backend GDI. Os IIDs
 * vem de -ldxguid (dado estatico, nao carrega DLL). Compilado a 0x0A00.
 *
 * Nota de teste (deep-gfx-server-windows): numa VM sem WDDM (BasicDisplay/
 * WARP) o flip roda em SOFTWARE e nao engata Independent Flip — o bypass real
 * do DWM so se valida com virtio-gpu-WDDM/passthrough. Aqui ele so precisa
 * apresentar (a costura e' a mesma do GDI).
 *
 * Referencia: MS "For best performance, use DXGI flip model"; roadmap em
 * docs/pesquisa/deep-gfx-server-windows.md.
 */
#define COBJMACROS
#include "present.h"
#include <d3d11.h>
#include <dxgi1_2.h>

typedef HRESULT (WINAPI *t_D3D11CreateDevice)(
    IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
    const D3D_FEATURE_LEVEL *, UINT, UINT,
    ID3D11Device **, D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);

static t_D3D11CreateDevice p_D3D11CreateDevice;

static int load_d3d(void)
{
    static int tried, ok;
    if (!tried) {
        tried = 1;
        HMODULE m = LoadLibraryA("d3d11.dll");
        if (m)
            p_D3D11CreateDevice =
                (t_D3D11CreateDevice)(void *)GetProcAddress(m, "D3D11CreateDevice");
        ok = (p_D3D11CreateDevice != NULL);
    }
    return ok;
}

typedef struct {
    ID3D11Device        *dev;
    ID3D11DeviceContext *ctx;
    IDXGISwapChain1     *swap;
    ID3D11Texture2D     *upload;   /* DYNAMIC, WRITE_DISCARD: sem stall no Map */
    ID3D11Texture2D     *back;     /* backbuffer cacheado (estavel no flip D3D11) */
    int w, h;
} DxgiImpl;

/* textura de upload DYNAMIC (referencia mmozeiko pixels.c) — DYNAMIC exige
 * um BindFlag; WRITE_DISCARD deixa o driver renomear o buffer (nunca trava). */
static int make_upload(DxgiImpl *g)
{
    D3D11_TEXTURE2D_DESC td;
    ZeroMemory(&td, sizeof td);
    td.Width = g->w;
    td.Height = g->h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    return SUCCEEDED(ID3D11Device_CreateTexture2D(g->dev, &td, NULL, &g->upload))
               ? 0 : -1;
}

static int get_backbuffer(DxgiImpl *g)
{
    return SUCCEEDED(IDXGISwapChain1_GetBuffer(g->swap, 0, &IID_ID3D11Texture2D,
                                               (void **)&g->back)) ? 0 : -1;
}

static int dxgi_init(PresentBackend *b, HWND root, int w, int h)
{
    DxgiImpl *g = (DxgiImpl *)b->impl;
    g->w = w;
    g->h = h;
    if (!load_d3d())
        return -1;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = p_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
                                     NULL, 0, D3D11_SDK_VERSION,
                                     &g->dev, &fl, &g->ctx);
    if (FAILED(hr))
        hr = p_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0,
                                 NULL, 0, D3D11_SDK_VERSION,
                                 &g->dev, &fl, &g->ctx);
    if (FAILED(hr))
        return -1;

    /* fabrica a partir do device (garante o mesmo adapter) */
    IDXGIDevice  *dxdev = NULL;
    IDXGIAdapter *adapter = NULL;
    IDXGIFactory2 *factory = NULL;
    int rc = -1;
    if (SUCCEEDED(ID3D11Device_QueryInterface(g->dev, &IID_IDXGIDevice,
                                              (void **)&dxdev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(dxdev, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetParent(adapter, &IID_IDXGIFactory2,
                                         (void **)&factory))) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        ZeroMemory(&sd, sizeof sd);
        sd.Width = w;
        sd.Height = h;
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.Scaling = DXGI_SCALING_NONE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        if (SUCCEEDED(IDXGIFactory2_CreateSwapChainForHwnd(
                factory, (IUnknown *)g->dev, root, &sd, NULL, NULL, &g->swap))) {
            IDXGIFactory2_MakeWindowAssociation(factory, root,
                                                DXGI_MWA_NO_ALT_ENTER);
            if (get_backbuffer(g) == 0 && make_upload(g) == 0)
                rc = 0;
        }
    }
    if (factory) IDXGIFactory2_Release(factory);
    if (adapter) IDXGIAdapter_Release(adapter);
    if (dxdev)   IDXGIDevice_Release(dxdev);
    return rc;
}

static void dxgi_present(PresentBackend *b, const Frame *f)
{
    DxgiImpl *g = (DxgiImpl *)b->impl;
    if (!g->swap || !g->upload || !g->back)
        return;

    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(ID3D11DeviceContext_Map(g->ctx, (ID3D11Resource *)g->upload,
                                          0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        const unsigned char *src = (const unsigned char *)f->bgra;
        unsigned char *dst = (unsigned char *)m.pData;
        int rows = f->h < g->h ? f->h : g->h;
        int rowbytes = (f->w < g->w ? f->w : g->w) * 4;
        for (int y = 0; y < rows; y++)
            memcpy(dst + (size_t)y * m.RowPitch, src + (size_t)y * f->stride,
                   (size_t)rowbytes);
        ID3D11DeviceContext_Unmap(g->ctx, (ID3D11Resource *)g->upload, 0);
    }

    ID3D11DeviceContext_CopyResource(g->ctx, (ID3D11Resource *)g->back,
                                     (ID3D11Resource *)g->upload);
    IDXGISwapChain1_Present(g->swap, 1, 0);   /* vsync */
}

static int dxgi_resize(PresentBackend *b, int w, int h)
{
    DxgiImpl *g = (DxgiImpl *)b->impl;
    g->w = w;
    g->h = h;
    if (g->back)   { ID3D11Texture2D_Release(g->back);   g->back = NULL; }
    if (g->upload) { ID3D11Texture2D_Release(g->upload); g->upload = NULL; }
    if (g->swap)
        IDXGISwapChain1_ResizeBuffers(g->swap, 0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (get_backbuffer(g) != 0)
        return -1;
    return make_upload(g);
}

static void dxgi_destroy(PresentBackend *b)
{
    DxgiImpl *g = (DxgiImpl *)b->impl;
    if (g->back)    ID3D11Texture2D_Release(g->back);
    if (g->upload)  ID3D11Texture2D_Release(g->upload);
    if (g->swap)    IDXGISwapChain1_Release(g->swap);
    if (g->ctx)     ID3D11DeviceContext_Release(g->ctx);
    if (g->dev)     ID3D11Device_Release(g->dev);
    free(g);
    free(b);
}

PresentBackend *present_dxgi_create(void)
{
    PresentBackend *b = (PresentBackend *)calloc(1, sizeof *b);
    DxgiImpl *g = (DxgiImpl *)calloc(1, sizeof *g);
    if (!b || !g) {
        free(b);
        free(g);
        return NULL;
    }
    b->name = "dxgi";
    b->init = dxgi_init;
    b->present = dxgi_present;
    b->resize = dxgi_resize;
    b->destroy = dxgi_destroy;
    b->impl = g;
    return b;
}
