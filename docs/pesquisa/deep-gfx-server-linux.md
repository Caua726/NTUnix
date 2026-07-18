# Deep dive: o stack gráfico/servidor de display do Linux — do pixel da app ao scanout físico

> Pesquisa técnica de fundamentação para o **dispd/displayd** do NTUnix (o servidor de display nativo).
> Objetivo: entender *de verdade*, ponta a ponta, como o mundo Linux leva pixels de uma aplicação até o
> painel físico — DRM/KMS no kernel, Mesa/GBM/EGL no user space, libinput/evdev para input, o protocolo
> Wayland, o mundo antigo do X para contraste, e o compartilhamento de buffers entre processos. No fim,
> um mapa "lições para o dispd" com os análogos NT (DXGI/D3DKMT/WDDM), amarrando com
> `nt-dwm-compositor.md`.
>
> **Método:** cada afirmação forte foi checada contra a *fonte real*. Onde possível, li os **headers
> UAPI/lib instalados nesta máquina** (Arch, kernel zen; libdrm 2.4.134, Mesa/GBM 26.1.4, libinput
> 1.31.3, wayland 1.25.0) — `/usr/include/drm/{drm.h,drm_mode.h,drm_fourcc.h}`, `/usr/include/gbm.h`,
> `/usr/include/libinput.h`, `/usr/include/wayland-server-core.h`, `/usr/include/EGL/*.h` — e corroborei
> a semântica de alto nível com a documentação do kernel, a doc de arquitetura do Wayland, o exemplo
> canônico `modeset-atomic.c` (dvdhrm/docs), assinaturas de libdrm e a doc do systemd-logind. As
> citações estão em §12.

---

## 0. Mapa mental em uma tela

O stack Linux moderno (Wayland) tem **um único processo privilegiado** que é dono da saída de vídeo e do
input — o **compositor**, que *é* o servidor de display. Não há um "servidor X" separado no meio. O
compositor:

1. abre o **device DRM** (`/dev/dri/card0`), vira **DRM master**, e fala **KMS** (Kernel Mode Setting)
   direto com o kernel para programar o hardware de scanout;
2. usa **GBM + Mesa/EGL** (ou Vulkan) para alocar buffers que a GPU pode renderizar *e* o display pode
   varrer, e para renderizar sua própria UI;
3. lê **input** via **libinput** (que lê `/dev/input/event*` — evdev);
4. fala o **protocolo Wayland** (via **libwayland-server**) com as aplicações-cliente, que renderizam em
   seus *próprios* buffers e os **compartilham** com o compositor via **dma-buf** (fd passado pelo socket)
   ou **wl_shm** (memória compartilhada);
5. compõe tudo e faz um **atomic commit** para o KMS; o kernel troca o buffer no **vblank** e devolve um
   **evento de page-flip**, que fecha o loop e agenda o próximo quadro.

```text
   ┌─────────────┐   Wayland wire (AF_UNIX + SCM_RIGHTS p/ passar fds)   ┌──────────────┐
   │  cliente A  │ ───── wl_surface.attach(wl_buffer)+damage+commit ───► │              │
   │ (GTK/Qt/…)  │       buffer = dma-buf fd  ou  wl_shm pool            │  COMPOSITOR  │
   └─────┬───────┘                                                        │  (= servidor │
         │ renderiza em renderD128 (render node, sem master)             │   de display)│
   ┌─────▼───────┐                                                        │              │
   │ Mesa (EGL/  │                                                        │  libinput ◄──┼── /dev/input/event*
   │  Vulkan/GL) │                                                        │  GBM/EGL     │
   └─────────────┘                                                        └──────┬───────┘
                                                                                 │ é DRM master de card0
                                                        drmModeAtomicCommit(FB_ID,…, PAGE_FLIP_EVENT)
                                                                                 ▼
   ┌──────────────────────────────── KERNEL (DRM/KMS) ──────────────────────────────────────┐
   │  Framebuffer ─► Plane(primary/overlay/cursor) ─► CRTC ─► Encoder ─► Connector ─► TELA    │
   │                       ▲ scanout no vblank ─────► devolve struct drm_event_vblank (flip)  │
   └─────────────────────────────────────────────────────────────────────────────────────────┘
```

Os componentes e onde vivem:

| Camada | Componente | Onde roda | Papel |
|---|---|---|---|
| Kernel | **DRM/KMS** (`drm.ko` + driver: `i915`, `amdgpu`, `nouveau`…) | kernel | modeset, gestão de memória GPU (GEM), scanout, vblank, dma-buf |
| Kernel | **evdev** (`evdev.ko`, `hid`, `input` core) | kernel | normaliza HID → `struct input_event` em `/dev/input/event*` |
| lib | **libdrm** | user | wrapper fino dos ioctls DRM (`drmMode*`, `drmPrime*`) |
| lib | **Mesa** (driver Gallium/Vulkan) + **GBM** + **EGL** | user | aloca buffers scanout-capazes, renderiza GL/GLES/Vulkan |
| lib | **libinput** | user | evdev cru → eventos de alto nível (aceleração, gestos, tap) |
| lib | **libwayland-server / -client** | user | loop de eventos + marshalling do wire protocol |
| serviço | **systemd-logind / seatd** | user (root) | dono de seats/VTs; entrega fds de DRM/evdev "mutáveis" por sessão |
| processo | **compositor** (wlroots/Weston/Mutter/KWin) | user | *o* servidor de display: junta tudo acima |

A grande virada arquitetural do Wayland está resumida na própria doc oficial: *"In wayland the compositor
**is** the display server. We transfer the control of KMS and evdev to the compositor"*, e o servidor X
passou a ser *"just a middle man that introduces an extra step between applications and the compositor and
an extra step between the compositor and the hardware"* (fonte: wayland.freedesktop.org/architecture).

---

## 1. DRM/KMS — o substrato do kernel

### 1.1 O device node, render node vs primary node, e o DRM master

Um GPU aparece como dois device nodes em `/dev/dri/`:

- **`card0`** (primary node) — permite **modeset (KMS)** e é protegido por **DRM master**. Só um cliente
  pode ser master de cada vez; sem ser master, os ioctls que mudam a tela falham com `EACCES`/`EPERM`.
- **`renderD128`** (render node) — só faz **render/compute + alocação de buffers**, *sem* modeset e *sem*
  master. Qualquer processo com permissão pode abrir e submeter trabalho à GPU. É por isso que **apps
  clientes renderizam no render node** e **só o compositor toca o primary node**. Essa separação é o
  fundamento de segurança do modelo: um cliente nunca controla o scanout.

**Virar DRM master.** Ao abrir `card0`, o primeiro cliente que faz `drmSetMaster()` (ioctl
`DRM_IOCTL_SET_MASTER` = `DRM_IO(0x1e)`) vira master; `drmDropMaster()` (`DRM_IOCTL_DROP_MASTER` =
`0x1f`) larga. Historicamente havia o esquema *magic cookie* (`DRM_IOCTL_GET_MAGIC`=`0x02`,
`DRM_IOCTL_AUTH_MAGIC`=`0x11`) em que o X autenticava clientes DRI2; hoje o master é gerido por
**logind/seat** (§3.3). Há também **DRM leases** (`drm_mode_create_lease`,
`DRM_IOCTL_MODE_CREATE_LEASE`) — criar um "sub-master" que recebe só um subconjunto de objetos (um CRTC +
connector + plane), usado por VR (headset direto) sem ceder a tela toda.

**Client capabilities** — antes de usar recursos modernos, o cliente liga capacidades com
`drmSetClientCap(fd, CAP, 1)` (`DRM_IOCTL_SET_CLIENT_CAP`=`0x0d`). Os que importam (de `drm.h`):

- `DRM_CLIENT_CAP_UNIVERSAL_PLANES` (2) — expõe *todos* os planes (primary/cursor/overlay) como objetos
  de primeira classe, não só overlays.
- `DRM_CLIENT_CAP_ATOMIC` (3) — habilita a API atômica (e implicitamente universal planes + expõe
  properties atômicas antes escondidas — `DRM_MODE_PROP_ATOMIC` = `0x80000000` esconde props de
  userspace legado).
- `DRM_CLIENT_CAP_WRITEBACK_CONNECTORS` (5), `DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT` (6, para VMs),
  `DRM_CLIENT_CAP_PLANE_COLOR_PIPELINE` (7, gestão de cor por-plane, novíssimo).

E capacidades **do driver** consultadas com `drmGetCap`/`DRM_IOCTL_GET_CAP` (`0x0c`): `DRM_CAP_DUMB_BUFFER`
(1), `DRM_CAP_PRIME` (5, import/export dma-buf), `DRM_CAP_TIMESTAMP_MONOTONIC` (6, timestamps de vblank em
`CLOCK_MONOTONIC` — sempre 1 desde kernel 4.15), `DRM_CAP_ASYNC_PAGE_FLIP` (7), `DRM_CAP_ADDFB2_MODIFIERS`
(0x10, framebuffers com format modifiers), `DRM_CAP_CRTC_IN_VBLANK_EVENT` (0x12, o `crtc_id` vem no
evento de flip — essencial p/ multi-monitor atômico), `DRM_CAP_SYNCOBJ`/`SYNCOBJ_TIMELINE` (0x13/0x14,
explicit sync).

### 1.2 O modelo de objetos KMS — o "pipeline" de display

O KMS expõe cinco tipos de objeto, cada um com um **object ID** de 32 bits. A doc do kernel descreve a
cadeia exatamente assim: *"Framebuffers feed into planes. Planes feed their pixel data into a CRTC for
blending"*, e o CRTC roteia por Encoder até o Connector.

```
    Framebuffer (drm_framebuffer)   → o quê exibir: (formato FourCC, modifier, handles[], pitches[], offsets[])
        │
        ▼
    Plane (drm_plane)               → uma camada de scanout do hardware
        │   tipos: DRM_PLANE_TYPE_PRIMARY / _CURSOR / _OVERLAY
        │   props: FB_ID, CRTC_ID, SRC_{X,Y,W,H} (16.16 fixo), CRTC_{X,Y,W,H}, zpos, rotation, alpha
        ▼
    CRTC (drm_crtc)                 → o "scanout engine": lê planes, faz blend, gera timing
        │   props: ACTIVE (on/off), MODE_ID (blob c/ o modo), GAMMA_LUT, CTM, DEGAMMA_LUT, VRR_ENABLED
        ▼
    Encoder (drm_encoder)           → converte o sinal do CRTC p/ o fio (TMDS/DP/LVDS/DSI). Hoje é
        │                             "internal artifact of the helper libraries" (doc kernel), quase
        │                             invisível ao userspace atômico
        ▼
    Connector (drm_connector)       → a porta física (HDMI-A-1, DP-1, eDP-1). Carrega EDID, status
                                      (connected/disconnected), lista de modos, e a prop CRTC_ID que o
                                      "pluga" num CRTC
```

Descoberta dos objetos: `drmModeGetResources()` (ioctl `DRM_IOCTL_MODE_GETRESOURCES`) devolve arrays de
IDs de CRTCs, connectors, encoders e FBs (`struct drm_mode_card_res`). `drmModeGetPlaneResources()`
(`struct drm_mode_get_plane_res`) lista os planes. Para cada objeto se consulta metadados:
`drm_mode_get_connector` (com o padrão *"ioctl duas vezes"* — primeira p/ obter contagens, segunda p/
preencher arrays; a doc alerta que é *racy* com hotplug, então "retry até estabilizar"), `drm_mode_crtc`,
`drm_mode_get_plane` (que traz `possible_crtcs`, o bitmask de quais CRTCs o plane pode alimentar).

Os IDs têm até *type tags* mágicos em `drm_mode.h`: `DRM_MODE_OBJECT_CRTC`=`0xcccccccc`,
`CONNECTOR`=`0xc0c0c0c0`, `ENCODER`=`0xe0e0e0e0`, `PLANE`=`0xeeeeeeee`, `FB`=`0xfbfbfbfb`,
`PROPERTY`=`0xb0b0b0b0`, `BLOB`=`0xbbbbbbbb`.

**Insight de arquitetura:** um monitor aceso = uma cadeia `Connector → CRTC → (N planes) → FBs`. Multi-
monitor = **N CRTCs independentes**, cada um com seu vblank. Planes são a aceleração de composição do
hardware: em vez de o compositor blendar tudo num FB e varrer, ele pode pôr o vídeo YUV num overlay
plane, o cursor num cursor plane e a UI no primary plane, e o **CRTC faz o blend no scanout** — zero cópia,
zero GPU para esse compositing. É o "hardware overlay/plane" que o Windows chama de MPO (Multi-Plane
Overlay).

### 1.3 Properties — a interface universal

Tudo em KMS atômico é **property**. Cada objeto tem props (`drm_mode_obj_get_properties`), e cada prop tem
um tipo (`drm_mode_get_property`): `DRM_MODE_PROP_RANGE`, `_ENUM`, `_BITMASK`, `_BLOB` (dados arbitrários,
ex. o modo de vídeo ou uma LUT de gama), `_OBJECT` (referência a outro objeto — é assim que `CRTC_ID` liga
um plane a um CRTC), `_SIGNED_RANGE`. Props imutáveis (`DRM_MODE_PROP_IMMUTABLE`) são read-only (ex.
`type` do plane).

**Blobs** são criados com `drm_mode_create_blob` (copia bytes, devolve `blob_id`) — o modo de vídeo vira
um blob (`struct drm_mode_modeinfo`) e vira o valor da prop `MODE_ID` do CRTC. Isso é o que permite "o
modo inteiro" ser um único valor atômico.

### 1.4 Legacy modeset vs Atomic — a diferença que define tudo

**Legacy (o mundo pré-2016):** ioctls imperativos, um por operação, cada um valida e aplica na hora:

- `drmModeSetCrtc(fd, crtcId, bufferId, x, y, connectors*, count, mode)` — liga um CRTC a um FB e um modo.
- `drmModePageFlip(fd, crtc_id, fb_id, flags, user_data)` — troca o FB do CRTC no próximo vblank.
- `drmModeSetPlane(...)` (`drm_mode_set_plane`) — move/atualiza um overlay.
- `drmModeSetCursor2()`, `drmModeMoveCursor()` — cursor (via `drm_mode_cursor2`, com hotspot).

Problema: não há transação. Você não consegue "mudar modo + FB + posição de 3 planes atomicamente"; cada
ioctl pode pegar ou falhar de forma independente, gerando **flicker** e estados intermediários inválidos.
E não há "posso fazer isto?" sem realmente tentar.

**Atomic (o mundo atual):** *uma* transação com *todas* as mudanças. A doc do kernel: o modelo força um
padrão *check-before-commit* — *"no hardware changes are allowed when the commit would fail"*. A libdrm
expõe três funções (assinaturas reais de `xf86drmMode.h`):

```c
drmModeAtomicReqPtr drmModeAtomicAlloc(void);
int  drmModeAtomicAddProperty(drmModeAtomicReqPtr req,
                              uint32_t object_id, uint32_t property_id, uint64_t value);
int  drmModeAtomicCommit(int fd, drmModeAtomicReqPtr req, uint32_t flags, void *user_data);
```

Você acumula (object_id, prop_id, value) no request e faz **um** commit. O ioctl por baixo é
`DRM_IOCTL_MODE_ATOMIC` (`0xBC`) com `struct drm_mode_atomic { flags; count_objs; objs_ptr;
count_props_ptr; props_ptr; prop_values_ptr; user_data; }` — arrays paralelos de objetos/props/valores.

**Flags do commit** (de `drm_mode.h`, combináveis):

- `DRM_MODE_ATOMIC_TEST_ONLY` (0x0100) — *"do not apply … instead check whether the hardware supports
  this configuration"*. Isto é ouro: dá pra perguntar ao driver "esta composição de planes cabe na
  banda?" sem tocar a tela. Compositores usam pra decidir quais surfaces vão pra hardware plane e quais
  precisam ser compostas na GPU.
- `DRM_MODE_ATOMIC_NONBLOCK` (0x0200) — retorna imediatamente; a mudança aplica no vblank e chega via
  evento. É o modo normal de um loop de compositor.
- `DRM_MODE_ATOMIC_ALLOW_MODESET` (0x0400) — permite operações caras/com artefato visível (trocar de
  modo, ligar/desligar CRTC). Sem esta flag, um commit que exigiria modeset falha com `EINVAL` — é uma
  trava de segurança pra não fazer modeset caro por acidente num page-flip.
- `DRM_MODE_PAGE_FLIP_EVENT` (0x01) — pede que o kernel mande de volta um `struct drm_event_vblank`
  quando o flip completar (um por CRTC envolvido).

**As propriedades de um quadro** (do `modeset-atomic.c` canônico): para acender e desenhar, você seta —
no **connector**: `CRTC_ID`; no **CRTC**: `MODE_ID` (blob) + `ACTIVE=1`; no **plane**: `FB_ID`, `CRTC_ID`,
`SRC_X/Y/W/H` (coordenadas na fonte, em **16.16 fixed-point** — por isso largura/altura vêm `<< 16`) e
`CRTC_X/Y/W/H` (retângulo destino na tela). Um **page-flip** de estado-estável é só re-setar `FB_ID` do
plane e commitar com `NONBLOCK|PAGE_FLIP_EVENT` — sem `ALLOW_MODESET`.

### 1.5 Framebuffers: registrando um buffer para scanout

Um buffer alocado (GEM/GBM/dumb) só vira scanout-able quando registrado como **framebuffer** KMS:

- `drmModeAddFB2(fd, w, h, pixel_format, handles[4], pitches[4], offsets[4], &fb_id, flags)` —
  `struct drm_mode_fb_cmd2`. `pixel_format` é um **FourCC** de `drm_fourcc.h` (ex. `DRM_FORMAT_XRGB8888`).
  Suporta **multi-plane** (YUV: Y em `offsets[0]`, UV em `offsets[1]`) com até 4 planos-de-memória.
- `drmModeAddFB2WithModifiers(...)` — igual, mas com `modifier[4]` e a flag `DRM_MODE_FB_MODIFIERS` (1<<1)
  ligada. **Todos os planos precisam do mesmo modifier.**
- `drmModeRmFB()` / `DRM_IOCTL_MODE_CLOSEFB` (`drm_mode_closefb`) — desregistra.

**Format modifiers** (crucial e mal-entendido) — o `pixel_format` FourCC só diz o *layout de pixel*
(XRGB8888). O **modifier** (u64) diz o *layout de memória*: linear vs tiled vs comprimido. Definições em
`drm_fourcc.h`:

- `DRM_FORMAT_MOD_LINEAR = fourcc_mod_code(NONE, 0)` — linha após linha, sem tiling. O que a CPU entende.
- `DRM_FORMAT_MOD_INVALID = fourcc_mod_code(NONE, DRM_FORMAT_RESERVED)` — "não sei / negocia por mim".
- Vendor-específicos: `I915_FORMAT_MOD_X_TILED`, `..._Y_TILED`, `..._Y_TILED_CCS` (Intel, com
  render-compression), `AMD_FMT_MOD` (AMD, campos bit-packed de tiling/DCC). `fourcc_mod_code(vendor,val)`
  = `((u64)vendor << 56) | (val & ((1<<56)-1))`; vendors: `NONE=0`, `INTEL=0x01`, `AMD=0x02`, etc.

Por que importa pro dispd: **um buffer tiled/comprimido tem throughput muito maior**, mas o *produtor*
(GPU render) e o *consumidor* (scanout, ou outro processo) precisam **concordar no modifier**. Toda a
negociação de dma-buf moderna (protocolo `zwp_linux_dmabuf`, EGL, Vulkan) troca **listas de (formato,
modifier) suportados** para achar a interseção. Modifier errado = imagem corrompida ou fallback lento.

### 1.6 Dumb buffers — o caminho sem GPU

Para quem não tem/quer aceleração, o KMS oferece **dumb buffers**: `drmIoctl(DRM_IOCTL_MODE_CREATE_DUMB,
&struct drm_mode_create_dumb{width,height,bpp})` devolve um `handle` GEM + `pitch` + `size`; depois
`DRM_IOCTL_MODE_MAP_DUMB` (`drm_mode_map_dumb`) dá um `offset` fake para `mmap()`, e você escreve pixels na
CPU. Sempre **linear**, sempre single-plane RGB. É o "framebuffer console" — perfeito para bring-up e para
o modo software do dispd. `DRM_CAP_DUMB_BUFFER` diz se há suporte; a doc recomenda `XRGB8888`/bpp 32 no
primary plane como o denominador comum.

### 1.7 Vblank & page-flip: o loop de realimentação

O relógio de todo compositor é o **vblank**. Você não faz busy-loop; você agenda e espera o evento.

Quando você commita com `DRM_MODE_PAGE_FLIP_EVENT`, ao completar o flip o kernel escreve no fd do DRM um
**`struct drm_event_vblank`**:

```c
struct drm_event_vblank {
    struct drm_event base;   // { type, length }; type = DRM_EVENT_FLIP_COMPLETE (0x02)
    __u64 user_data;         // o ponteiro que você passou no commit — normalmente o "output"
    __u32 tv_sec, tv_usec;   // timestamp do flip (CLOCK_MONOTONIC)
    __u32 sequence;          // contador de frames (msc)
    __u32 crtc_id;           // qual CRTC (0 em kernels antigos; requer CRTC_IN_VBLANK_EVENT)
};
```

O compositor põe o **fd do DRM no seu event loop** (o `epoll`/`poll` principal). Quando o fd fica legível,
chama `drmHandleEvent(fd, &ev)`, onde `ev` é um `drmEventContext` com `version=3` e o callback
`page_flip_handler2(fd, seq, tv_sec, tv_usec, crtc_id, user_data)`. A libdrm lê o `drm_event_vblank`,
identifica o tipo e dispara o callback certo. O `crtc_id` é o que permite, num setup multi-monitor com
atomic, saber **qual output** completou (por isso `DRM_CAP_CRTC_IN_VBLANK_EVENT`).

**O ciclo (estado-estável):**

```
   renderiza quadro N  →  registra/reusa FB  →  drmModeAtomicCommit(FB_ID=N, NONBLOCK|PAGE_FLIP_EVENT)
        ▲                                                                    │
        │                                                                    ▼   (no vblank, hw troca)
   page_flip_handler2(crtc_id)  ◄── drmHandleEvent ◄── fd legível ◄── kernel escreve drm_event_vblank
        │
        └─ "flip N confirmado; libera o FB antigo; começa a desenhar N+1"
```

Regra de ouro: **um flip pendente por CRTC**. Se você commitar de novo antes do evento, toma `EBUSY`.
O evento é o *sinal de crédito* para o próximo quadro — é o que casa a taxa do compositor com a taxa do
monitor (o equivalente ao "present/vsync throttle"). `DRM_MODE_PAGE_FLIP_ASYNC` (0x02) pede flip imediato
(pode rasgar) — usado por jogos "tearing/low-latency". Há ainda `DRM_MODE_PAGE_FLIP_TARGET_ABSOLUTE/RELATIVE`
para agendar o flip num `sequence` de vblank específico.

### 1.8 GEM handles e explicit sync (contexto)

Buffers da GPU são objetos **GEM** (Graphics Execution Manager). Um **GEM handle** é um `u32` **válido só
dentro do fd que o abriu** — não é global, não atravessa processos. É por isso que o compartilhamento usa
**PRIME/dma-buf** (§7). Sincronização entre GPU e display historicamente foi **implícita** (fences
atrelados ao dma-buf, o kernel serializa); o mundo moderno migra para **explicit sync** via
**`drm_syncobj`** (timelines de fence explícitas, `DRM_CAP_SYNCOBJ_TIMELINE`), que o Vulkan e o protocolo
`wp_linux_drm_syncobj` usam para evitar stalls e microtravadas.

---

## 2. Mesa / GBM / EGL — do render ao framebuffer de scanout

O KMS sabe *varrer* um buffer, mas não sabe *alocar* um buffer que a GPU consiga renderizar. Quem faz a
ponte é o **GBM (Generic Buffer Manager)** — uma lib do Mesa. A doc do próprio header resume: *"provides an
abstraction … to request a buffer from the underlying memory management system"*, e a comunidade descreve
GBM como *"a middleman between EGL and DRM, managing buffers for rendering"*.

### 2.1 GBM: alocar buffers scanout-capazes

Assinaturas reais de `/usr/include/gbm.h`:

```c
struct gbm_device  *gbm_create_device(int fd);                 // fd = o MESMO fd do KMS (card0)
struct gbm_surface *gbm_surface_create(struct gbm_device *gbm,
                        uint32_t width, uint32_t height,
                        uint32_t format, uint32_t flags);      // format = GBM_FORMAT_XRGB8888, etc.
struct gbm_surface *gbm_surface_create_with_modifiers2(..., const uint64_t *modifiers, unsigned count, uint32_t flags);
struct gbm_bo      *gbm_surface_lock_front_buffer(struct gbm_surface *s);
void                gbm_surface_release_buffer(struct gbm_surface *s, struct gbm_bo *bo);
```

As **flags de uso** (`enum gbm_bo_flags`) declaram a intenção e restringem a alocação para algo que sirva a
todos os consumidores:

- `GBM_BO_USE_SCANOUT` (1<<0) — *"going to be presented to the screen using an API such as KMS"* → o
  buffer precisa estar em memória e layout que o display engine consiga varrer.
- `GBM_BO_USE_RENDERING` (1<<2) — servirá de color buffer da GPU.
- `GBM_BO_USE_CURSOR` (1<<1), `GBM_BO_USE_LINEAR` (1<<4), `GBM_BO_USE_PROTECTED` (1<<5, conteúdo DRM/HDCP),
  `GBM_BO_USE_FRONT_RENDERING`, e flags de fixed-rate compression.

Um buffer de scanout do compositor é tipicamente `GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING` — a
interseção que é *ao mesmo tempo* renderável pela GPU e varrível pelo display. Essa é a razão de existir do
GBM: nem toda memória renderável é scanout-able (alinhamento, contiguidade, tiling suportado pelo display).

De um `gbm_bo` você extrai o que o KMS precisa:

```c
union gbm_bo_handle gbm_bo_get_handle(struct gbm_bo *bo);       // GEM handle p/ drmModeAddFB2
int      gbm_bo_get_fd(struct gbm_bo *bo);                       // dma-buf fd p/ compartilhar (PRIME)
uint32_t gbm_bo_get_stride(struct gbm_bo *bo);                   // pitch
uint64_t gbm_bo_get_modifier(struct gbm_bo *bo);                // o modifier real que o driver escolheu
int      gbm_bo_get_plane_count(struct gbm_bo *bo);             // multi-plane
uint32_t gbm_bo_get_offset(struct gbm_bo *bo, int plane);
```

O fluxo: `gbm_bo_get_handle()` + `gbm_bo_get_stride()` + `gbm_bo_get_modifier()` → `drmModeAddFB2WithModifiers()`
→ `fb_id` → prop `FB_ID` no atomic commit. Na prática você faz isso **uma vez por bo** e cacheia o
`fb_id` (o `gbm_surface` recicla um pequeno pool de bos, tipo 3–4).

### 2.2 EGL sobre GBM: o loop de render→present

O EGL amarra a GPU (GL/GLES) ao GBM. O compositor cria o EGLDisplay a partir do device GBM usando a
**plataforma GBM** (`EGL_PLATFORM_GBM_KHR = 0x31D7`, extensão `EGL_KHR_platform_gbm`/`EGL_MESA_platform_gbm`):

```c
EGLDisplay dpy = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm_device, NULL);
eglInitialize(dpy, ...);
EGLSurface surf = eglCreateWindowSurface(dpy, config, (EGLNativeWindowType)gbm_surface, NULL);
```

O **render loop** por output (padrão que todo compositor KMS segue):

```
   eglMakeCurrent(dpy, surf, surf, ctx);
   … desenha a cena (a UI do compositor + as texturas das apps) …
   eglSwapBuffers(dpy, surf);                       // GPU escreve no back buffer do gbm_surface
   struct gbm_bo *bo = gbm_surface_lock_front_buffer(gbm_surface);  // pega o buffer recém-completo
   uint32_t fb = fb_id_for_bo(bo);                  // drmModeAddFB2 (cacheado)
   drmModeAtomicCommit(fd, req{plane.FB_ID=fb}, NONBLOCK|PAGE_FLIP_EVENT, output);
   // … no page_flip_handler2, quando o flip do quadro ANTERIOR confirma:
   gbm_surface_release_buffer(gbm_surface, bo_anterior);  // devolve ao pool p/ reuso
```

A doc do Mesa é explícita: *"gbm_surface_lock_front_buffer() must be called exactly once after calling
eglSwapBuffers"* e o bo *"should later be returned to the gbm surface using gbm_surface_release_buffer()"*.
O `gbm_surface` deixa o EGL alocar os buffers, mas **deixa você (o compositor) no comando do present via
KMS** — que é exatamente a divisão de responsabilidade que o dispd quer.

Para **compor as janelas das apps** (não só renderizar UI própria), o compositor importa os buffers dos
clientes como texturas: um dma-buf de cliente vira `EGLImage` via `eglCreateImageKHR(...,
EGL_LINUX_DMA_BUF_EXT, ...)` (extensão `EGL_EXT_image_dma_buf_import[_modifiers]`), que vira uma textura GL
(`glEGLImageTargetTexture2DOES`) usada no shader de composição — **zero cópia** (§7.3).

### 2.3 Vulkan (o caminho alternativo)

Em vez de EGL/GBM, um compositor Vulkan usa a **WSI**: `VK_KHR_display` +
`VK_KHR_display_swapchain` (present direto a um KMS display), ou renderiza offscreen e importa/exporta via
`VK_KHR_external_memory_fd` / `VK_EXT_image_drm_format_modifier` (o análogo Vulkan dos modifiers). O
`vkQueuePresentKHR` acaba, por baixo, fazendo um atomic commit/page-flip. Conceitualmente idêntico; só
troca a lib de userspace.

### 2.4 Render node vs card node, na prática

- **Cliente**: abre `renderD128`, cria `gbm_device`/EGL/Vulkan ali, renderiza no *seu* buffer, exporta um
  **dma-buf fd**, manda pro compositor pelo socket Wayland. Nunca precisa de master nem de root.
- **Compositor**: abre `card0` (via logind), vira master, cria `gbm_device` no fd do card, importa os
  dma-bufs dos clientes, compõe, e faz scanout.

Essa assimetria é a base do isolamento: o kernel garante que um cliente não consegue mexer no modeset nem
ler o framebuffer de outra app.

---

## 3. libinput / evdev — input do kernel ao compositor

### 3.1 evdev: a fonte crua

O kernel abstrai todo dispositivo de input (teclado, mouse, touchpad, touchscreen, tablet, gamepad) como
um **evdev node** `/dev/input/eventN`. Você lê `struct input_event { struct timeval time; __u16 type;
__u16 code; __s32 value; }`. `type` ∈ {`EV_KEY` (teclas/botões), `EV_REL` (movimento relativo — mouse),
`EV_ABs` (posição absoluta — touch/tablet), `EV_SYN` (fim de um pacote de eventos), …}. Um clique de mouse
= `EV_KEY code=BTN_LEFT value=1` seguido de `EV_SYN`. É funcional, mas **cru demais**: sem aceleração de
ponteiro, sem detecção de tap/scroll/gesto, sem calibração — cada compositor teria que reimplementar isso.

### 3.2 libinput: a camada de política

**libinput** consome os evdev nodes e entrega **eventos de alto nível**, com aceleração de ponteiro,
natural scrolling, tap-to-click, gestos de dois/três dedos, palm rejection, etc. É a lib que Weston,
wlroots, Mutter e KWin usam. Fluxo real de `/usr/include/libinput.h`:

**1) Criar o contexto** — dois modos:

```c
struct libinput_interface {
    int  (*open_restricted)(const char *path, int flags, void *user_data);   // você abre o fd (via logind)
    void (*close_restricted)(int fd, void *user_data);
};
struct libinput *libinput_udev_create_context(const struct libinput_interface *iface,
                                              void *user_data, struct udev *udev);
int  libinput_udev_assign_seat(struct libinput *li, const char *seat_id);    // ex. "seat0"
// ou, sem udev, apontando devices na mão:
struct libinput *libinput_path_create_context(const struct libinput_interface *iface, void *user_data);
struct libinput_device *libinput_path_add_device(struct libinput *li, const char *path);
```

O ponto-chave: libinput **não abre os devices ela mesma** — ela chama seu `open_restricted()`, e é aí que
o compositor pede o fd ao **logind** (que devolve um fd "mutável", cortado quando a sessão está inativa —
§3.3). Isso é o que permite o compositor rodar **sem root**.

**2) Integrar no event loop** — libinput expõe **um único fd** que agrega tudo:

```c
int fd = libinput_get_fd(struct libinput *li);   // põe no epoll/poll principal
// quando fd fica legível:
libinput_dispatch(li);                            // lê os evdev e processa internamente
struct libinput_event *ev;
while ((ev = libinput_get_event(li)) != NULL) {
    switch (libinput_event_get_type(ev)) {        // enum libinput_event_type
    case LIBINPUT_EVENT_POINTER_MOTION:        …  libinput_event_get_pointer_event(ev)  …
    case LIBINPUT_EVENT_POINTER_BUTTON:        …
    case LIBINPUT_EVENT_KEYBOARD_KEY:          …  libinput_event_get_keyboard_event(ev) …
    case LIBINPUT_EVENT_TOUCH_DOWN:            …
    case LIBINPUT_EVENT_DEVICE_ADDED:          …  // hotplug
    }
    libinput_event_destroy(ev);
}
```

Os tipos de evento (valores reais): `LIBINPUT_EVENT_DEVICE_ADDED`, `..._KEYBOARD_KEY = 300`,
`..._POINTER_MOTION = 400`, `..._POINTER_MOTION_ABSOLUTE`, `..._POINTER_BUTTON`,
`..._POINTER_SCROLL_WHEEL`, `..._TOUCH_DOWN`, etc. O compositor então roteia: pega a posição do ponteiro,
consulta seu **scene graph** para achar a surface sob o cursor, converte para coordenadas locais da
janela, e manda o evento **direto ao cliente** via Wayland (`wl_pointer.motion`, `wl_keyboard.key`).

### 3.3 seat / logind — quem entrega os fds

Compositor moderno **não é root** e **não abre `/dev/dri/*` nem `/dev/input/*` diretamente**. Ele fala com
o **systemd-logind** (ou o `seatd`/`elogind` para não-systemd), via D-Bus:

- `org.freedesktop.login1.Session.TakeControl()` — vira o controlador da sessão.
- `TakeDevice(major, minor)` — devolve um **fd** para aquele device. A doc do logind: o fd é **"muted"**
  automaticamente quando a sessão fica inativa (VT-switch para outra sessão), e "resumed" ao reativar. Para
  DRM, o fd entregue a uma sessão **inativa** vem **sem o bit MASTER**; para evdev, o kernel oferece
  `EVIOCREVOKE`. Isso garante — nas palavras da doc — que *"a device can never be used by anyone else than
  the foreground session"*.
- Em VT-switch, o compositor é notificado (`PauseDevice`/`ResumeDevice` signals), larga o DRM master
  (`drmDropMaster`) e para de desenhar; ao voltar, refaz `drmSetMaster` e um modeset completo.

Isso é o **seat management**: um "seat" (`seat0`) = um conjunto de devices (GPU + inputs) atribuído a uma
sessão. `libseat` abstrai logind/seatd atrás de uma API única, e o compositor usa `libseat_open_device()`
no lugar de `open()` no `open_restricted()`.

---

## 4. Wayland — o protocolo e a libwayland

### 4.1 Modelo: objetos, requests, events, o registry

Wayland é um **protocolo orientado a objeto sobre um socket AF_UNIX**. Cada objeto tem um **ID de 32 bits**
e uma **interface** (ex. `wl_surface`, `wl_pointer`, `wl_output`). A comunicação é assimétrica:

- **requests** = mensagens **cliente → servidor** (métodos que o cliente invoca).
- **events** = mensagens **servidor → cliente** (notificações).

O cliente conecta e obtém o objeto raiz **`wl_display`** (ID 1, sempre). Dele pede o **`wl_registry`**, que
**anuncia os globals** — os serviços que o compositor oferece (`wl_compositor`, `wl_shm`, `wl_seat`,
`wl_output`, `xdg_wm_base`, `zwp_linux_dmabuf_v1`…), cada um com nome numérico + interface + versão. O
cliente faz **bind** nos que quer. Esse é o mecanismo de descoberta e versionamento; nada é hard-coded.

**O wire format** (por que é barato): cada mensagem é uma sequência de **palavras de 32 bits**:
`[object_id][opcode(16) | tamanho(16)][argumentos…]`. Argumentos: inteiros, fixed 24.8, strings/arrays
(com length + padding a 4 bytes), object IDs, **new_id** (o cliente aloca o ID do novo objeto — sem
round-trip), e **fd** (não vai no fluxo de bytes; viaja como **ancillary data SCM_RIGHTS** do socket
Unix — §7). Marshalling é copiar inteiros num buffer; sem XML em runtime, sem parsing de texto. O XML só
existe em *build time*: `wayland-scanner` gera o código C de stubs a partir das descrições de protocolo.

### 4.2 libwayland-server: o loop de eventos e o marshalling

O lado servidor (o compositor) usa **libwayland-server**. As peças reais de
`/usr/include/wayland-server-core.h`:

```c
struct wl_display     *wl_display_create(void);
struct wl_event_loop  *wl_display_get_event_loop(struct wl_display *);
const char            *wl_display_add_socket_auto(struct wl_display *);  // cria XDG_RUNTIME_DIR/wayland-N
void                   wl_display_run(struct wl_display *);              // o loop principal
void                   wl_display_flush_clients(struct wl_display *);

struct wl_event_loop  *wl_event_loop_create(void);
struct wl_event_source*wl_event_loop_add_fd(struct wl_event_loop *loop, int fd, uint32_t mask,
                                            wl_event_loop_fd_func_t func, void *data);
int                    wl_event_loop_dispatch(struct wl_event_loop *loop, int timeout);
int                    wl_event_loop_get_fd(struct wl_event_loop *loop);  // é um epoll fd!
```

O **`wl_event_loop` é um reator single-thread sobre epoll**. Tudo vira uma *event source*: o socket de
escuta Wayland, cada fd de cliente, o **fd do DRM** (page-flip events), o **fd do libinput**, timers
(`wl_event_loop_add_timer`), sinais (`wl_event_loop_add_signal`), idle callbacks. `wl_display_run()` é um
`while (running) wl_event_loop_dispatch(loop, -1);`. Como `wl_event_loop_get_fd()` devolve um epoll fd, dá
até para **aninhar** o loop do Wayland dentro de outro. **Um compositor Wayland é, no fundo, um único loop
de eventos epoll** costurando KMS + input + IPC dos clientes.

**Globals, clientes e recursos:**

```c
struct wl_global   *wl_global_create(struct wl_display *, const struct wl_interface *iface,
                                     int version, void *data, wl_global_bind_func_t bind);
struct wl_client   *wl_client_create(struct wl_display *, int fd);       // no accept() do socket
struct wl_resource *wl_resource_create(struct wl_client *, const struct wl_interface *, int version, uint32_t id);
void  wl_resource_set_implementation(struct wl_resource *, const void *impl, void *data, wl_resource_destroy_func_t);
void  wl_resource_post_event(struct wl_resource *, uint32_t opcode, ...);  // manda um event ao cliente
int   wl_client_get_credentials(struct wl_client *, pid_t *, uid_t *, gid_t *);  // SO_PEERCRED
```

Um **`wl_resource`** é a materialização, no servidor, de um objeto que o cliente criou; ele carrega uma
tabela de implementação (os handlers de cada request) via `wl_resource_set_implementation`. Quando o
cliente chama `wl_surface.attach`, a libwayland desempacota a mensagem e chama seu callback C. Quando você
quer notificar o cliente (`wl_pointer.motion`), chama `wl_resource_post_event`. `wl_client_get_credentials`
(via `SO_PEERCRED`) dá pid/uid do cliente — base para políticas de segurança (quem pode usar screencopy,
etc.).

### 4.3 O ciclo de vida de uma superfície (o "commit" de novo)

Do lado da app, mostrar conteúdo é:

1. `wl_compositor.create_surface()` → um `wl_surface`.
2. dar um papel: `xdg_wm_base.get_xdg_surface(surface)` + `xdg_surface.get_toplevel()` (uma janela).
3. criar um buffer (`wl_shm` ou `zwp_linux_dmabuf`), renderizar nele.
4. `wl_surface.attach(wl_buffer, 0, 0)` + `wl_surface.damage(x,y,w,h)` (o que mudou) +
   `wl_surface.commit()`.

**Wayland tem "atomic" também, no nível da surface:** `attach`+`damage`+transform+scale ficam em *pending
state*; só o `commit` os aplica de uma vez (double-buffered state). Isso espelha exatamente o atomic
commit do KMS — nenhuma composição vê meia-atualização. Para **pacing**, o cliente pede um
**`wl_surface.frame()`** callback: o compositor dispara esse callback quando é bom desenhar de novo
(tipicamente atrelado ao vblank), e o cliente só então renderiza o próximo quadro. É o equivalente
client-side do page-flip event — throttle sem busy-loop.

### 4.4 Damage tracking

Como o cliente informa **exatamente qual retângulo mudou** (`wl_surface.damage`), o compositor pode
recompor só o necessário e — se o driver suportar — passar isso ao KMS via a prop de plane
`FB_DAMAGE_CLIPS` (blob de `struct drm_mode_rect`) para atualização parcial de scanout. Menos GPU, menos
banda, menos energia. A doc do Wayland: *"the application must tell the compositor which area of the surface
holds new contents"*.

---

## 5. O mundo antigo (X11) — para contraste

O X.Org Server é um **processo separado** entre clientes e hardware. Sua arquitetura é dividida em:

- **DIX (Device-Independent X)** — o núcleo portável: protocolo X11, gestão de clientes/recursos,
  despacho, extensões. *"machine and device independent part of X"*.
- **DDX (Device-Dependent X)** — a camada específica de hardware/SO: driver de vídeo (o `xf86-video-*` com
  a ABI de driver do X, ou hoje o driver genérico `modesetting` que fala KMS), input, inicialização de
  tela. O DDX *"translates abstract requests from the DIX into vendor-specific operations"*.

O caminho gráfico evoluiu:

- **UMS → KMS (2008):** com DRI2, o modeset saiu do user space (driver do X) para o kernel (KMS) — a raiz
  do DRM/KMS que vemos hoje.
- **DRI2:** o **X server aloca** os render buffers e os compartilha com o cliente via **GEM names**
  (inseguros: um número global adivinhável, qualquer cliente podia acessar).
- **DRI3 + Present (2013, Keith Packard):** os **clientes** alocam os buffers e os passam ao X via
  **PRIME dma-buf fds** (seguro, baseado em fd, não em nome global). O **Present** é *"a way to get new
  window content from a pixmap to the screen in a VBLANK-synchronized way"* — o análogo X do page-flip.
- **GLX** liga OpenGL ao X; **glamor** acelera as chamadas 2D do X via OpenGL/shaders.

**O contraste que importa para o dispd:** no X há **três processos** no caminho (cliente → X server →
compositor) e o input passa **pelo X server** antes de chegar ao cliente — round-trips e cópias
redundantes. A doc do Wayland ataca isso diretamente: o X vira *"a middle man"*. **O modelo Wayland é o
alvo do dispd**: um processo (o dispd) *é* o servidor de display, sem intermediário; clientes alocam e
compartilham buffers, o servidor só compõe e apresenta. Esse é também, não por acaso, o design do
DWM/DirectComposition no Windows (um compositor que é dono do scanout; apps entregam surfaces) — ver §11.

---

## 6. (reservado — consolidado em §7)

---

## 7. Compartilhamento de buffers entre processos — a mecânica

Este é o coração de qualquer servidor de display: como os pixels de uma app chegam ao compositor **sem
cópia**. Dois caminhos.

### 7.1 dma-buf + PRIME — o zero-copy da GPU

O problema: um **GEM handle** só vale dentro do fd DRM que o criou (§1.8). A solução é o **dma-buf**: um
objeto de buffer do kernel, referenciado por um **file descriptor** — e fds *atravessam* processos.

- **Exportar** (produtor): `DRM_IOCTL_PRIME_HANDLE_TO_FD` (`0x2d`) — `struct drm_prime_handle { handle;
  flags; fd; }`. Dá um GEM handle, recebe um **dma-buf fd** (com `flags = DRM_CLOEXEC | DRM_RDWR`). Em GBM:
  `gbm_bo_get_fd()`.
- **Passar o fd** ao compositor: como **SCM_RIGHTS** — o Wayland embute o fd na mensagem
  (`zwp_linux_buffer_params_v1.add(fd, plane_idx, offset, stride, modifier_hi, modifier_lo)`), e a
  libwayland o envia como *ancillary data* (`sendmsg`/`cmsg`) pelo socket Unix. O kernel **duplica o fd**
  no processo receptor apontando para o **mesmo** dma-buf.
- **Importar** (consumidor, o compositor): `DRM_IOCTL_PRIME_FD_TO_HANDLE` (`0x2e`) → novo GEM handle local
  para o mesmo buffer; ou, mais alto nível, `gbm_bo_import(gbm, GBM_BO_IMPORT_FD_MODIFIER,
  &gbm_import_fd_modifier_data{ width,height,format, num_fds, fds[4], strides[4], offsets[4], modifier },
  flags)`; ou direto para textura via `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)`.

**A negociação de formato/modifier** é o que faz isso robusto: o compositor anuncia, pelo protocolo
`zwp_linux_dmabuf_v1`, a lista de `(DRM_FORMAT_*, modifier)` que ele consegue **scanout ou textura**; o
cliente intersecta com o que a GPU dele consegue **produzir**; alocam no modifier comum. Se o resultado
for scanout-able, o compositor pode pôr direto num hardware plane (**direct scanout**, zero composição). Se
não, importa como textura e compõe na GPU. É exatamente o loop `TEST_ONLY` do §1.4 decidindo isso quadro a
quadro.

```
   CLIENTE (renderD128)                          SOCKET (AF_UNIX)                COMPOSITOR (card0)
   gbm_bo (render)                                                              
     └ gbm_bo_get_fd() ─► dma-buf fd ── zwp_linux_dmabuf params.add(fd,off,stride,mod) ─► SCM_RIGHTS
                                                                                  │
                                          wl_surface.attach(wl_buffer)+commit ───►│
                                                                                  ▼
                                                      import: gbm_bo_import / eglCreateImage(DMA_BUF)
                                                                                  │
                                            direct scanout (hw plane)  OU  textura no shader de composição
```

Sincronização: implícita (fence anexado ao dma-buf; `DMA_BUF_IOCTL_SYNC` para acesso CPU) ou explícita
(`drm_syncobj` + `wp_linux_drm_syncobj` — o cliente entrega um fence de "render pronto" e recebe um de
"pode reusar o buffer").

### 7.2 wl_shm — o caminho de memória compartilhada (CPU)

Para clientes sem GPU (ou toolkits software), Wayland tem **wl_shm**: o cliente cria um **fd de memória**
(`memfd_create`/`shm_open`), `ftruncate` para o tamanho do pool, `mmap` local, e manda o **fd pelo socket**
(SCM_RIGHTS) via `wl_shm.create_pool(fd, size)`. O compositor faz `mmap` do **mesmo** fd — agora ambos veem
os mesmos bytes. O cliente pinta os pixels na CPU, `wl_surface.attach`+`commit`, e o servidor lê:

```c
struct wl_shm_buffer *wl_shm_buffer_get(struct wl_resource *buffer_resource);
void  wl_shm_buffer_begin_access(struct wl_shm_buffer *);   // proteção SIGBUS (se o cliente truncar o pool)
void *wl_shm_buffer_get_data(struct wl_shm_buffer *);
int32_t wl_shm_buffer_get_stride/format/width/height(...);
int   wl_display_init_shm(struct wl_display *);             // registra o global wl_shm
```

É **uma cópia** (o compositor faz upload do shm para uma textura GPU antes de compor), mas é simples,
sem-driver e à prova de balas — o caminho de bring-up. `wl_shm_buffer_begin_access` existe porque o pool é
memória do cliente: se ele encolher o pool sob os pés do servidor, o acesso viraria SIGBUS; a libwayland
instala um handler para transformar isso em erro de protocolo em vez de crash.

### 7.3 Por que isso é o gargalo de design

O caminho **dma-buf** é o que dá 4K@120Hz sem fritar CPU: o buffer é escrito pela GPU do cliente e lido
pelo display engine **sem nunca passar pela CPU nem ser copiado**. O `wl_shm` é o fallback universal. Um
servidor de display sério precisa dos **dois**, com a negociação de modifier no meio. Guardar isto para
o §11: no NT, o análogo direto do dma-buf-fd-por-socket é o **NT handle duplicável** para uma **seção/
surface D3DKMT compartilhada** (`D3DKMTShareObjects` / `D3DKMTOpenResource` / handle NT dup via
`DuplicateHandle`), e o análogo do `wl_shm` é uma **section object** (`NtCreateSection` +
`NtMapViewOfSection`) mapeada nos dois processos.

---

## 8. A vida de um quadro — ponta a ponta, com os nomes reais

Juntando tudo, um quadro no mundo Wayland/KMS (compositor tipo wlroots):

1. **Input chega** — o usuário mexe o mouse. Kernel escreve `input_event` em `/dev/input/event5`. O **fd do
   libinput** (`libinput_get_fd`) fica legível no epoll do compositor → `libinput_dispatch()` →
   `libinput_get_event()` → `LIBINPUT_EVENT_POINTER_MOTION`. O compositor atualiza a posição do cursor,
   acha a surface sob ele no scene graph, e faz `wl_resource_post_event(wl_pointer, motion, …)` **direto ao
   cliente**.
2. **Cliente renderiza** — recebe o motion, decide redesenhar. Renderiza na GPU (render node), termina com
   um buffer novo (`gbm_bo`), exporta `gbm_bo_get_fd()`.
3. **Cliente apresenta** — `wl_surface.attach(wl_buffer)` (o buffer criado via `zwp_linux_dmabuf`, com o fd
   já enviado por SCM_RIGHTS), `wl_surface.damage(...)`, `wl_surface.commit()`. Isso viaja pelo socket; o
   fd do cliente fica legível no epoll do compositor.
4. **Compositor recebe** — `wl_event_loop_dispatch` desempacota a mensagem, chama o handler de
   `surface.commit`. O compositor **importa** o dma-buf (`gbm_bo_import`/`eglCreateImage`) — zero cópia.
5. **Compositor decide o plano** — monta um atomic request de **teste**
   (`drmModeAtomicCommit(..., DRM_MODE_ATOMIC_TEST_ONLY)`): "cabe pôr esta surface num overlay plane
   direto?". Se sim → **direct scanout** (FB_ID do plane = o buffer do cliente). Se não → compõe na GPU
   (desenha a textura do cliente + sua UI no `gbm_surface`, `eglSwapBuffers`,
   `gbm_surface_lock_front_buffer`).
6. **Compositor apresenta** — monta o request real: props `FB_ID` (do buffer composto ou do cliente),
   `CRTC_ID`, `SRC_*`, `CRTC_*`, e commita:
   `drmModeAtomicCommit(fd, req, DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT, output)`.
7. **Kernel/hardware** — no próximo **vblank**, o CRTC troca para o novo framebuffer (page flip). O display
   engine varre os pixels para o painel.
8. **Realimentação** — o kernel escreve um `struct drm_event_vblank` (type `DRM_EVENT_FLIP_COMPLETE`,
   `crtc_id`, timestamp `CLOCK_MONOTONIC`, `sequence`) no fd do DRM. O **fd do DRM** fica legível no epoll →
   `drmHandleEvent()` → `page_flip_handler2(crtc_id, …)`. O compositor: libera o buffer antigo
   (`gbm_surface_release_buffer` / manda `wl_buffer.release` ao cliente para ele poder reusar), dispara os
   `wl_surface.frame` callbacks (destrava os clientes para o próximo quadro), e **agenda o próximo commit**.

O **vblank event é o metrônomo**. Todo o resto (input, render, composição) é orquestrado por ele através de
um **único loop epoll**. Não há thread de render separada obrigatória; há um reator.

---

## 9. Síntese: os invariantes que um servidor de display precisa honrar

1. **Um dono do scanout por output.** DRM master (ou VidPN owner no NT). Só ele programa o hardware.
2. **Separação produtor/consumidor de buffers.** Clientes alocam e renderizam em nós sem privilégio;
   compartilham por handle/fd; o servidor importa. Nunca o cliente toca o scanout.
3. **Transação atômica de estado.** Nada de meia-atualização visível — nem no KMS (atomic commit) nem na
   surface (commit double-buffered). Ter um **TEST_ONLY** para validar sem aplicar é enorme.
4. **Present é ligado ao vblank, e o vblank realimenta.** O evento de flip é crédito para o próximo quadro
   (pacing, throttling, `wl_surface.frame`).
5. **Negociação de formato+modifier.** O layout de memória (tiling/compressão) precisa ser acordado entre
   quem produz e quem varre; senão, corrupção ou cópia lenta.
6. **Planes de hardware = composição grátis.** Descarregar overlay/cursor/vídeo para planes evita GPU.
7. **Um loop de eventos costura tudo:** socket de clientes + fd do display (flip events) + fd de input +
   timers. Reator single-thread epoll.
8. **Seat/sessão gerencia posse.** Quando a sessão fica inativa (VT-switch), o servidor larga o master e
   os fds ficam "muted"; ao voltar, refaz master + modeset.

---

## 10. Onde o dispd difere do Linux (a fronteira NT)

O dispd **não terá DRM/KMS**. No NTUnix, o kernel é o NT e o driver é **WDDM**, cuja interface de kernel é
**`dxgkrnl.sys`** (DirectX Graphics Kernel), acessada de user space pelas thunks **D3DKMT** em `gdi32.dll`
(as `D3DKMT*`). O "libdrm" do NT é o conjunto D3DKMT; o "GBM/EGL/Mesa" é **DXGI + D3D**; o "atomic commit/
page flip" é **`D3DKMTPresent`** (ou a swap chain flip-model do DXGI); o "DRM master" é
**`D3DKMTSetVidPnSourceOwner`**; o "evdev/libinput" é **Raw Input / HID**; o "dma-buf/PRIME" é o
**handle NT de recurso D3D compartilhado**. O `nt-dwm-compositor.md` já mapeou o caminho de *posse da
saída* (flip-model 3b / VidPN owner 3d); esta pesquisa fornece o **modelo de referência completo** que o
dispd deve imitar em cima dessas primitivas.

---

## 11. Lições para o dispd — o mapa NT (DXGI / D3DKMT / WDDM)

### 11.1 Tabela de analogias

| Conceito Linux (o que li) | Função/objeto real | Análogo NT para o dispd | Notas de projeto |
|---|---|---|---|
| Device node de GPU | `/dev/dri/card0` (primary) vs `renderD128` (render) | Adaptador DXGI (`IDXGIAdapter`) + kernel `\\.\GraphicsCard`; D3D device (render) vs VidPN source (scanout) | A separação **render vs scanout** existe no NT: um app cria um **D3D device** (render) sem ser dono da saída; só o compositor vira **VidPN source owner** |
| Virar DRM master | `drmSetMaster` / `DRM_IOCTL_SET_MASTER` | `D3DKMTSetVidPnSourceOwner(EXCLUSIVE / EXCLUSIVEGDI)` ou flip-model fullscreen do DXGI | É a **posse da saída**. Ver `nt-dwm-compositor.md` §3d(i). Só um dono por output |
| DRM leases (sub-master) | `drm_mode_create_lease` | (sem análogo direto; IddCx cria *tela virtual*, não sublease) | Guardar como pesquisa; VR headset direto não é prioridade |
| Pipeline Framebuffer→Plane→CRTC→Connector | objetos KMS | **VidPN** (Video Present Network): VidPnSource → path → VidPnTarget; MPO = **DXGI multiplane overlay** | O WDDM tem o mesmo grafo: source (buffer) → target (conector). Modelar o dispd nesse grafo |
| Hardware planes (overlay/cursor) | `DRM_PLANE_TYPE_*`, `SRC_*`/`CRTC_*` | **DXGI multiplane overlay** (`IDXGIOutput->CheckOverlaySupport`, `IDXGISwapChain1->Present` com overlays) + hardware cursor | Descarregar vídeo/cursor para overlay = mesma economia. Checar suporte por-output |
| Atomic commit (transação) | `drmModeAtomicCommit(req, flags)` | `IDXGISwapChain::Present`(flip model) / `D3DKMTPresent`; e o **DirectComposition** commit (`IDCompositionDevice::Commit`) para a árvore de visuais | O DirectComposition é *literalmente* o "atomic state commit" da árvore de composição do NT — o análogo do surface.commit + KMS atomic juntos |
| `DRM_MODE_ATOMIC_TEST_ONLY` | validar sem aplicar | `CheckOverlaySupport` / `CheckPresentDurationSupport` / testar via `D3DKMTCheckMultiPlaneOverlaySupport` | Muito útil: decidir por-quadro o que vai pra overlay |
| Framebuffer + FourCC + modifier | `drmModeAddFB2WithModifiers`, `DRM_FORMAT_*`, `DRM_FORMAT_MOD_*` | `DXGI_FORMAT_*` + **tiling/swizzle implícito do WDDM** (não exposto como "modifier" público) | **Diferença dura:** o NT esconde o modifier atrás do runtime. Buffers compartilhados D3D carregam o layout no metadata do recurso; a negociação some. Simplifica o dispd, mas tira controle fino |
| dma-buf fd por socket (PRIME) | `PRIME_HANDLE_TO_FD`/`FD_TO_HANDLE`, SCM_RIGHTS | **Shared resource NT handle**: `D3DKMTShareObjects`/`CreateSharedHandle` (`ID3D11Device::OpenSharedResource`, DXGI `IDXGIResource1::CreateSharedHandle`) + `DuplicateHandle` entre processos | O NT handle é o "fd". Passa por **ALPC/named pipe** em vez de socket Unix, mas a ideia (um handle kernel que aponta o mesmo buffer) é idêntica |
| `wl_shm` (memória compartilhada) | `memfd` + mmap + SCM_RIGHTS | **Section object**: `NtCreateSection` + `NtMapViewOfSection` nos dois processos (ou `CreateFileMapping`) | Caminho de bring-up sem GPU. Trivial no NT |
| evdev + libinput | `/dev/input/event*`, `libinput_dispatch`/`_get_event` | **Raw Input** (`RegisterRawInputDevices`, `WM_INPUT`, `GetRawInputBuffer`) ou HID (`hid.dll`, `\\?\HID#…`) | `nt-dwm-compositor.md` §5 já recomenda Raw Input buffered. libinput também faz **política** (aceleração, gestos) — o dispd precisará dessa camada por cima do Raw Input |
| seat/logind (posse de device por sessão) | `TakeControl`/`TakeDevice`, muted fds, VT-switch | **Session/WinSta isolation** do NT: sessões (Session 0/1+), `WTS*`, `SetProcessWindowStation`; posse da saída cedida/retomada no fast-user-switch | O NT já isola por sessão. VT-switch ↔ trocar de sessão/desktop; largar/retomar VidPN owner |
| vblank / page-flip event | `drm_event_vblank`, `drmHandleEvent`, `page_flip_handler2` | **DXGI**: `IDXGIOutput::WaitForVBlank`, `IDXGISwapChain::GetFrameStatistics`, waitable swap chain (`GetFrameLatencyWaitableObject`); `D3DKMTWaitForVerticalBlankEvent` | O "evento de flip como crédito" vira o **waitable object** do DXGI. Mesmo padrão de pacing |
| Wayland wire (objetos/requests/events) | libwayland, `wl_resource_post_event` | **Protocolo próprio do NTUnix** sobre **ALPC** (nativo, rápido) ou **AF_UNIX**/named pipe; opcodes binários | Não reusar Wayland-protocol as-is (amarra a wl_shm/dmabuf Linux). Copiar o *modelo* (registry/globals/versão, requests/events, new_id client-aloca) sobre transporte NT |
| Passar fd/handle na mensagem | SCM_RIGHTS | **`DuplicateHandle`** com o pid do peer, ou handle via ALPC `LPC_HANDLE` | O peer credential (`wl_client_get_credentials`) vira `NtQueryInformationProcess`/`GetNamedPipeClientProcessId` |
| Event loop (reator epoll) | `wl_event_loop` + epoll | **IOCP** (`CreateIoCompletionPort`) + `NtAssociateWaitCompletionPacket`, ou `MsgWaitForMultipleObjects` | Costurar: pipe/ALPC de clientes + waitable swap chain (vblank) + Raw Input + timers num IOCP. `VISAO.md` §6 já mapeia epoll→IOCP |
| Damage tracking | `wl_surface.damage`, `FB_DAMAGE_CLIPS` | DXGI `Present1` com **dirty rects** (`DXGI_PRESENT_PARAMETERS.pDirtyRects`) + scroll rect | Suporte de primeira classe no DXGI. Casar o "damage" do protocolo do dispd com os dirty rects |
| Surface commit atômico (client) | `attach`+`damage`+`commit` double-buffered | **DirectComposition** visual tree + `Commit()`, ou o próprio protocolo do dispd com pending/commit | Manter a semântica "sem meia-atualização" |
| Explicit sync | `drm_syncobj`, `wp_linux_drm_syncobj` | **D3D fences** (`ID3D12Fence`/`ID3D11Fence`, `CreateSharedHandle`) | Fence compartilhado entre cliente e dispd para evitar cópia/tearing |
| dumb buffer (bring-up sem GPU) | `DRM_IOCTL_MODE_CREATE_DUMB` + mmap | GDI DIB / `D3DKMTCreateAllocation` linear + map; ou **Basic Display Adapter** (rasterizador CPU) | O primeiro dispd pode desenhar num buffer linear e apresentar via flip-model — o "modo dumb" |

### 11.2 Recomendações concretas de arquitetura para o dispd

1. **Adote o modelo "compositor É o servidor de display"** (Wayland), não o modelo X (servidor separado).
   Um processo `dispd` dono da saída + do protocolo + do input. Bate com a conclusão de
   `nt-dwm-compositor.md` (compositor próprio via flip-model, não substituir o dwmcore).

2. **Tome posse da saída pelo caminho público primeiro** (DXGI flip-model fullscreen/borderless — 3b do
   `nt-dwm-compositor.md`), que é o análogo estável do "virar DRM master". Evolua para
   `D3DKMTSetVidPnSourceOwner` (3d) só quando precisar de mais controle/latência — é o análogo do master
   cru. **Um dono por output**, exatamente como o DRM.

3. **Separe render de scanout como o Linux separa card/render node.** Clientes criam D3D devices e
   renderizam nas próprias surfaces; **nunca** tocam o present final. Só o dispd apresenta. Isso é o
   isolamento de segurança de graça.

4. **Compartilhe buffers por handle NT, não por cópia.** O análogo do dma-buf-fd é o **shared resource
   handle** D3D (`CreateSharedHandle` + `OpenSharedResource`), duplicado para o dispd via `DuplicateHandle`
   (ou passado por ALPC). Tenha o fallback **section object** (análogo do `wl_shm`) para clientes sem GPU e
   para bring-up. Implemente **os dois** desde cedo.

5. **Faça o present ser atômico e vblank-driven.** Use a **waitable swap chain** do DXGI
   (`GetFrameLatencyWaitableObject`) como o "page-flip event" — é o crédito para o próximo quadro. Costure
   esse waitable + input + IPC num **IOCP** (o "epoll" do NT). Um reator.

6. **Modele a árvore de composição no DirectComposition** se quiser overlays/planes de hardware de graça
   (o MPO do WDDM), ou componha na GPU (D3D) você mesmo. O `TEST_ONLY` do KMS vira
   `CheckOverlaySupport`/`CheckMultiPlaneOverlaySupport` — decida por-quadro o que vai pra overlay.

7. **Desenhe um protocolo próprio inspirado no Wayland** (registry/globals com versão; requests
   cliente→servidor e events servidor→cliente; new_id alocado pelo cliente; surface pending/commit
   double-buffered; frame callback para pacing; damage/dirty-rects) — mas **sobre transporte NT** (ALPC ou
   named pipe/AF_UNIX), com **DuplicateHandle** no lugar de SCM_RIGHTS. Não amarre ao wire do Wayland (ele
   pressupõe dma-buf/wl_shm Linux); copie o *design*, não os bytes.

8. **Input:** Raw Input buffered (público, baixa latência — `nt-dwm-compositor.md` §5) como o "evdev", com
   uma camada de **política** por cima (aceleração de ponteiro, gestos de touchpad, tap) = o papel do
   libinput. O roteamento (achar a janela sob o cursor, converter para coordenadas locais, mandar ao
   cliente) é idêntico ao do compositor Wayland.

9. **Gestão de sessão:** ceder/retomar a posse da saída no fast-user-switch/lock, análogo ao largar/retomar
   DRM master no VT-switch. O NT já isola por Session/WindowStation; o dispd só precisa reagir aos eventos
   de ativação de sessão.

10. **Bring-up incremental (espelhando o Linux):** (a) *modo dumb* — desenhar num buffer linear na CPU e
    apresentar via flip-model (análogo dumb buffer + `wl_shm`); (b) *composição GPU* — importar surfaces de
    clientes por handle e compor em D3D; (c) *overlays de hardware* — descarregar vídeo/cursor para MPO;
    (d) *explicit sync* com fences D3D compartilhados. Cada passo mapeia 1:1 num degrau do stack Linux que
    esta pesquisa descreveu.

---

## 12. Fontes

**Headers UAPI/lib lidos diretamente nesta máquina** (fonte primária, versões instaladas):
- `/usr/include/drm/drm.h`, `/usr/include/drm/drm_mode.h`, `/usr/include/drm/drm_fourcc.h` — libdrm 2.4.134
  (estruturas UAPI: `drm_mode_atomic`, `drm_mode_fb_cmd2`, `drm_event_vblank`, `drm_prime_handle`,
  `drm_mode_create_dumb`; ioctls `SET_MASTER`/`DROP_MASTER`/`PRIME_*`/`MODE_ATOMIC`/`MODE_PAGE_FLIP`;
  `DRM_CLIENT_CAP_*`, `DRM_CAP_*`; `DRM_MODE_ATOMIC_*`/`PAGE_FLIP_*` flags; `fourcc_mod_code`,
  `DRM_FORMAT_MOD_LINEAR/INVALID`, `I915_FORMAT_MOD_*`, `AMD_FMT_MOD`).
- `/usr/include/gbm.h` — Mesa/GBM 26.1.4 (`gbm_create_device`, `gbm_surface_create[_with_modifiers2]`,
  `gbm_surface_lock_front_buffer`/`release_buffer`, `gbm_bo_get_handle/fd/stride/modifier/offset`,
  `enum gbm_bo_flags` incl. `GBM_BO_USE_SCANOUT/RENDERING`, `GBM_BO_IMPORT_FD_MODIFIER`).
- `/usr/include/libinput.h` — libinput 1.31.3 (`libinput_udev_create_context`, `libinput_udev_assign_seat`,
  `libinput_path_create_context`/`add_device`, `libinput_get_fd`, `libinput_dispatch`,
  `libinput_get_event`, `libinput_event_get_type`, `struct libinput_interface{open_restricted,
  close_restricted}`, `enum libinput_event_type`).
- `/usr/include/wayland-server-core.h` — wayland 1.25.0 (`wl_display_create`/`run`/`add_socket_auto`,
  `wl_event_loop_*` incl. `add_fd`/`dispatch`/`get_fd`, `wl_global_create`, `wl_client_create`,
  `wl_resource_create`/`set_implementation`/`post_event`, `wl_client_get_credentials`,
  `wl_shm_buffer_get_data/stride/format`, `wl_display_init_shm`).
- `/usr/include/EGL/egl.h`, `/usr/include/EGL/eglext.h` — EGL 1.5 (`eglGetPlatformDisplay`,
  `EGL_PLATFORM_GBM_KHR=0x31D7`, `EGL_KHR_platform_gbm`, `eglCreateImageKHR`,
  `EGL_EXT_image_dma_buf_import[_modifiers]`).

**Documentação e código de referência (web):**
- Kernel.org — *Kernel Mode Setting (KMS)* / DRM internals: pipeline Framebuffer→Plane→CRTC→Encoder→
  Connector; modelo de properties; atomic check-before-commit; `TEST_ONLY`/`NONBLOCK`/`ALLOW_MODESET`;
  estágios `hw_done`/`flip_done`/`cleanup_done`. https://www.kernel.org/doc/html/latest/gpu/drm-kms.html
- wayland.freedesktop.org — *Wayland Architecture*: "the compositor is the display server"; X como "middle
  man"; input routing; buffer sharing; damage. https://wayland.freedesktop.org/architecture.html
- dvdhrm/docs — `drm-howto/modeset-atomic.c` (exemplo canônico): `drmSetClientCap(ATOMIC/UNIVERSAL_PLANES)`,
  `drmModeAtomicAlloc`/`AddProperty`, props `CRTC_ID`/`MODE_ID`/`ACTIVE`/`FB_ID`/`SRC_*`/`CRTC_*`,
  `drmModeAtomicCommit(PAGE_FLIP_EVENT|NONBLOCK)`, `drmHandleEvent` + `page_flip_handler2`.
  https://github.com/dvdhrm/docs/blob/master/drm-howto/modeset-atomic.c
- libdrm `xf86drmMode.h` — assinaturas: `drmModeAtomicAlloc/AddProperty/Commit`, `drmModeSetCrtc`,
  `drmModePageFlip`, `drmModeAddFB2[WithModifiers]`.
  https://cgit.freedesktop.org/drm/libdrm/tree/xf86drmMode.h
- systemd `org.freedesktop.login1` — `TakeControl`/`TakeDevice`, fds "muted" em sessão inativa, DRM sem bit
  MASTER para sessão inativa, `EVIOCREVOKE`, sincronização com VT-switch.
  https://freedesktop.org/software/systemd/man/org.freedesktop.login1.html ; contexto de design:
  https://3bb.cc/blog/2021/02/21/linux_console_graphics/
- Mesa — *EGL* (plataformas drm/gbm/surfaceless) e regra do `gbm_surface_lock_front_buffer` (exatamente uma
  vez após `eglSwapBuffers`; devolver com `release_buffer`). https://docs.mesa3d.org/egl.html ;
  `EGL_MESA_platform_gbm`: https://registry.khronos.org/EGL/extensions/MESA/EGL_MESA_platform_gbm.txt
- X.Org — arquitetura DIX/DDX; UMS→KMS; DRI2 (server aloca, GEM names) → DRI3 (client aloca, PRIME dma-buf)
  + Present (present VBLANK-sincronizado); glamor/GLX. XFree86 DDX Design:
  https://www.x.org/releases/X11R7.6/doc/xorg-server/DESIGN.html ; DRI3 & Present:
  https://lwn.net/Articles/569701/ ; https://en.wikipedia.org/wiki/X.Org_Server
- wlroots `backend/drm/atomic.c` — uso real de `drmModeAtomicCommit` com `DRM_MODE_PAGE_FLIP_EVENT`/
  `NONBLOCK` e handling de eventos DRM no event loop.
  https://github.com/swaywm/wlroots/blob/master/backend/drm/atomic.c

**Documentos internos do NTUnix referenciados:**
- `docs/pesquisa/nt-dwm-compositor.md` — DWM/DirectComposition, flip-model (3b), D3DKMT/VidPN owner (3d),
  Raw Input (§5). (o "outro lado" — o que esta pesquisa fundamenta com o modelo Linux de referência)
- `docs/VISAO.md` §17–§18 (ambiente gráfico), §6 (epoll→IOCP), §8 (DRM/KMS→WDDM/DXGI).
