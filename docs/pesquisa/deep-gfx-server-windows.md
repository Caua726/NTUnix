# Deep dive: o stack gráfico/display do Windows moderno (WDDM) — do pixel da app ao scanout

> Pesquisa técnica profunda para fundamentar o **display server nativo do NTUnix ("dispd")**.
> Objetivo: entender *exatamente* como o Windows 10/11 (WDDM) leva pixels de uma app até o painel físico, para construir o `dispd` que apresenta **hoje via GDI BitBlt (M1)**, **depois via DXGI flip-model (M3)**, e **eventualmente via D3DKMT** (M4+).
> Complementa e aprofunda `docs/pesquisa/nt-dwm-compositor.md` (a "verdade honesta" sobre substituir o DWM) e `docs/VISAO.md` §17–§18.
> Regra: cada afirmação forte tem fonte primária Microsoft. Termos de API preservados em inglês.

---

## 0. Sumário executivo (o mapa em uma tela)

O caminho "pixel → foton" no Windows moderno tem **6 camadas** que precisamos separar mentalmente, porque `dispd` vai entrar em pontos diferentes conforme evolui:

```
 APP (user-mode)
   |  desenha em: back buffer D3D  /  window DC (GDI)
   v
 [1] PRODUTOR de superfície por-janela
     - D3D/DXGI  -> swap chain (back buffers compartilhados)
     - GDI       -> CDD (cdd.dll) rasteriza p/ redirection surface
   |
   v
 [2] DirectComposition: árvore de visuais (dcomp.dll na app)  --marshalling-->
     win32k object database (kernel, por-sessão)  -->  dwmcore.dll (no dwm.exe)
   |
   v
 [3] COMPOSITOR (dwm.exe + dwmcore): compõe a árvore inteira do desktop
     -> 1 flip chain fullscreen por monitor
   |
   v
 [4] DXGI/Direct3D runtime -> gdi32 thunks -> D3DKMT* (user-mode)
   |
   v
 [5] dxgkrnl.sys (DirectX Graphics Kernel): VidMm + VidSch + display port
     -> agenda o present, programa o flip
   |
   v
 [6] KMD (display miniport) -> DxgkDdiSetVidPnSourceAddress -> hardware de scanout
     -> DAC/HDMI/DP -> PAINEL
```

**Onde o DWM entra e por que NÃO o substituímos:** a camada [3] lê as *redirection surfaces* de **todas** as janelas (camada [1]) por um contrato **100% interno** win32k↔dwmcore. Não há API pública *consumidora* para enumerar/ler essas surfaces. Isso já foi estabelecido em `nt-dwm-compositor.md` §1.3/§3a e continua verdade. Portanto `dispd` **não** vira o compositor do Win32 — ele **toma posse da saída de vídeo** (camadas [4]→[6]) e desenha *nossas* janelas nativas, deixando o DWM/Win32 numa camada isolada por baixo (ou nem iniciando o DWM).

**As três entradas do `dispd`, mapeadas nas camadas:**
- **M1 (GDI BitBlt):** entra em [1] via GDI. Só chega ao painel *sem passar pelo DWM* quando **não há DWM** (WinPE, sessão de boot). É o caminho mais simples para "primeiro pixel na tela" e para o console/bootstrap.
- **M3 (DXGI flip-model):** entra em [4] via swap chain flip. Em fullscreen/borderless-que-cobre-o-monitor, o *Independent Flip* faz [3] "dormir" e a nossa swap chain vira dona do scanout — mesmo mecanismo de qualquer jogo. É o backend gráfico "de verdade".
- **M4+ (D3DKMT):** entra direto em [4]/[5] via `D3DKMTSetVidPnSourceOwner` + `D3DKMTPresent`, sem o runtime DXGI no meio. Máximo controle, mínimo de dependência, mais risco.

---

## 1. O compositor: DWM (dwm.exe + dwmcore.dll) e DirectComposition

### 1.1 As peças e onde rodam

| Componente | Onde | Papel |
|---|---|---|
| `dwm.exe` | user-mode, **1 por sessão** (Session 1+) | processo hospedeiro do compositor |
| `dwmcore.dll` | dentro do `dwm.exe` | **a engine de composição** de fato (a "composition engine" do DirectComposition) |
| `dcomp.dll` | user-mode, em **cada app** | API COM pública do DirectComposition; a app monta sua *árvore de visuais* aqui |
| `win32kbase.sys` / `win32kfull.sys` | kernel, **1 por sessão** | window manager (NtUser) + GDI (NtGdi) + **object database do DirectComposition** que faz marshalling app→engine |
| `cdd.dll` (Canonical Display Driver) | kernel | rasterizador de software que recebe GDI e escreve em bitmap; a superfície é lida pelo compositor |
| `dxgkrnl.sys` (+ `dxgmms2.sys`) | kernel | DirectX Graphics Kernel: agenda GPU, gerencia vidmem, programa o present/scanout |

Fonte primária da divisão de papéis (dcomp / dwmcore / win32k / instanciação por-sessão) — [DirectComposition, Architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components):

- *"A user-mode composition engine (`dwmcore.dll`) that is hosted in the Desktop Window Manager (DWM) process (`dwm.exe`)."*
- *"A user-mode application library (`dcomp.dll`) that implements the Component Object Model (COM)-based public API."*
- *"A kernel-mode object database (part of `win32k.sys`) that marshals commands from the application to the composition engine. The kernel object database … stores all state information associated with the API calls."*
- *"Both the kernel-mode object database and the user-mode composition engine are **instantiated once per session** … A **single instance of the composition engine** handles the DirectComposition composition trees for all applications and the DWM composition tree, which represents the entire desktop."*

**Implicação #1 para o NTUnix:** o object database no win32k espera **exatamente uma** composition engine por sessão (o dwmcore). Colocar um "segundo dwmcore" implicaria falsificar o protocolo interno win32k↔dwmcore — engenharia reversa de contrato não-documentado e versionado a cada build. Por isso `dispd` **não** é uma composition engine alternativa; ele é dono do *scanout*, não do *object database de composição*.

### 1.2 A árvore de visuais e o marshalling

O modelo DirectComposition é retido (retained-mode): a app usa `dcomp.dll` para criar `IDCompositionDevice` → `IDCompositionVisual`s (com transform, clip, effect, e um **bitmap content**) → `IDCompositionTarget` ligado a um HWND. Cada mutação (`SetContent`, `SetTransform`, `AddVisual`, `Commit`) é serializada e **marshalled** pelo win32k object database para o dwmcore, que mantém a árvore-espelho no processo do DWM e a compõe por vblank.

O "bitmap" de um visual não é necessariamente um bitmap: a doc de [Bitmap objects (DirectComposition)](https://learn.microsoft.com/en-us/windows/win32/directcomp/bitmap-surfaces) explica que a engine troca esse placeholder em tempo real por uma de três coisas — *a composition surface, uma DXGI swap chain, ou a redirection surface de outra janela*. É assim que a swap chain flip-model de uma app (camada [1] D3D) entra na árvore do DWM **sem cópia**.

### 1.3 Redirection surfaces — o coração do problema (e por que ler é privado)

Cada janela top-level ganha uma **redirection surface** off-screen. A app desenha ali; o dwmcore lê as surfaces de TODAS as janelas e compõe.

- **App GDI:** as chamadas GDI são roteadas ao **CDD** (`cdd.dll`), um **rasterizador de software** que escreve num buffer (histórico: RAM no Vista, "aperture memory" a partir do Win7) do tamanho da janela; esse buffer é convertido/mantido em sincronia com uma DirectX surface em vidmem, que o compositor usa como textura da malha da janela. (Confirmação múltipla, incl. [Wikipedia — DWM](https://en.wikipedia.org/wiki/Desktop_Window_Manager): *"under DWM, GDI calls are redirected to use the Canonical Display Driver (cdd.dll), a software renderer … a buffer equal to the size of the window is allocated in system memory and CDD.DLL outputs to this buffer rather than the video memory … The surface is read by the compositor and is composited to the desktop in video memory."*)
- **App D3D/DXGI:** o back buffer é **compartilhado** com o DWM via `dxgkrnl` (zero cópia no flip model — ver §2).

**Por que ler as redirection surfaces de fora é impossível por API pública** (isto é o núcleo de por que NÃO substituímos o DWM — já detalhado em `nt-dwm-compositor.md` §1.3, resumo aqui):

- A única API documentada que toca redirection surface é **`DwmDxGetWindowSharedSurface`** — e é a face **produtora** (o runtime D3D *escreve* na surface da SUA janela). É Win7-only, driver-only, *"An application may not call this method"*, sem header, **ordinal 100 em `dwmapi.dll`** ([ref](https://learn.microsoft.com/en-us/windows/win32/dwm/dwmdxgetwindowsharedsurface)).
- **Não existe API consumidora** para enumerar e ler as surfaces de *todas* as janelas — isso é interno ao dwmcore.
- A única leitura sancionada do resultado composto é a **Desktop Duplication API**, que entrega o **desktop inteiro já composto**, nunca por-janela ([ref](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api)).

**Consequência arquitetural travada:** `dispd` compõe **apenas conteúdo nativo do NTUnix**. Se um dia quisermos mostrar janelas Win32 dentro do nosso compositor, o único caminho sancionado é Desktop Duplication (desktop Win32 inteiro como uma textura) — grosso mas possível. Substituir o dwmcore fica como P&D de longuíssimo prazo.

### 1.4 Como o DWM entrega o quadro final (o elo com [4]–[6])

O loop do dwmcore roda um quadro por vblank e no fim faz *"Present the frame by flipping the back and front buffers for each screen … Each monitor is represented by a separate **full-screen flip chain**"* ([Architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)). Ou seja: **o DWM detém uma flip chain fullscreen por monitor**, e o present dele desce pelo mesmo caminho DXGI→`D3DKMTPresent`→`dxgkrnl`→scanout que descrevemos em §2–§3. **Quem controla a flip chain do monitor controla o scanout.** Em operação normal é o DWM; em fullscreen exclusive / independent flip, passa a ser a app (§2.4). É exatamente essa transferência de posse que `dispd` M3/M4 explora.

---

## 2. DXGI + o flip model (o backend M3 do `dispd`)

### 2.1 Blt model vs flip model — a diferença no kernel

Fonte: [DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model).

> *"The biggest difference between bitblt and flip presentation models is how back-buffer contents get to the … DWM for composition. In the bitblt model, contents of the back buffer are **copied into the redirection surface** on each call to `IDXGISwapChain1::Present1`. In the flip model, all back buffers are **shared with the Desktop Window Manager (DWM)**. Therefore, the DWM can compose straight from those back buffers without any additional copy operations."*

Tabela oficial (present windowed):

| Passo | Blt model | Flip model |
|---|---|---|
| 1 | app escreve o frame | app escreve o frame |
| 2 | runtime **copia** a surface p/ a redirection surface do DWM (Read+Write) | runtime **passa** a surface p/ o DWM (sem cópia) |
| 3 | DWM compõe p/ tela (Read+Write) | DWM compõe p/ tela (Read+Write) |

O "flip" é literal: em vez de copiar, o runtime **rotaciona qual handle é o front buffer** na hora do present. Da mesma doc: *"the runtime creates one extra back buffer and **rotates whichever handle belongs to the buffer that becomes the front buffer** at presentation time."*

### 2.2 Os swap effects (a escolha que trava tudo)

Enum [`DXGI_SWAP_EFFECT`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect):

- `DXGI_SWAP_EFFECT_DISCARD` (0) — blt legado. **Não usar.**
- `DXGI_SWAP_EFFECT_SEQUENTIAL` (1) — blt legado. **Não usar.**
- `DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL` (3) — flip; **preserva** o conteúdo dos back buffers entre `Present`s.
- `DXGI_SWAP_EFFECT_FLIP_DISCARD` (4) — flip; **não garante** preservação → habilita a otimização "reverse composition" (o DWM pode "rabiscar" nos buffers da app).

Recomendação da Microsoft ([For best performance, use DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model)): *"If you are still using `DXGI_SWAP_EFFECT_DISCARD` or `DXGI_SWAP_EFFECT_SEQUENTIAL` … it's time to stop!"*

**Requisitos do flip model** (mesma doc):
1. `BufferCount` ≥ 2 (2 a 16; a doc de flip recomenda >2 p/ não esperar o DWM liberar o buffer anterior).
2. Depois de `Present`, **re-bindar** o back buffer no contexto imediato do D3D11.
3. Depois de `SetFullscreenState`, chamar `ResizeBuffers` antes de `Present`.
4. **Sem MSAA** direto — resolver o MSAA antes do `Present`.
5. `Format` ∈ {`R16G16B16A16_FLOAT`, `B8G8R8A8_UNORM`, `R8G8B8A8_UNORM`}; `SampleDesc` = {1,0}.
6. **Uma** swap chain flip por HWND; não misturar GDI/outro D3D no mesmo HWND (o runtime ignora updates blt/GDI naquele HWND).

### 2.3 DirectFlip, Independent Flip e reverse composition (o bypass do DWM)

Este é o mecanismo que `dispd` M3 usa para tomar o scanout. Da doc [For best performance…]:

Três cenários, em ordem crescente de funcionalidade (todos "DirectFlip"):
1. **DirectFlip:** buffers da swap chain **== dimensões da tela** e a client region da janela **cobre a tela**. Em vez da swap chain do DWM, usa-se a **swap chain da app** para exibir. *"Instead of using the DWM swapchain to display on the screen, the application swapchain is used."*
2. **DirectFlip com panel fitters:** a janela cobre a tela e os buffers estão dentro de um fator de escala (ex. 0.25x–4x); o **hardware de scanout** escala. (Cuidado: cursor pode esticar — `DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_CURSOR_STRETCHED`.)
3. **DirectFlip com MPO (multi-plane overlay):** o DWM reserva um **plano de scanout dedicado** para a app, escaneado e possivelmente esticado/alpha-blended numa sub-região.

Uma vez "DirectFlipped": *"the DWM can go to sleep, and only wake up when something changes outside of your application. Your application frames are sent directly to the screen, independently, **with the same efficiency as fullscreen exclusive**. This is **Independent Flip**."* Se algo aparece por cima, o DWM pode voltar a compor, fazer **reverse compose** (compor o de cima sobre os buffers da app antes do flip), ou usar MPO para manter independent flip.

Consulta de capacidade de composição por hardware: [`IDXGIOutput6::CheckHardwareCompositionSupport`](https://learn.microsoft.com/en-us/windows/win32/api/DXGI1_6/nf-dxgi1_6-idxgioutput6-checkhardwarecompositionsupport). Diagnóstico de qual cenário engatou: **PresentMon**.

**Escala e resize:** controle via [`DXGI_SCALING`](https://learn.microsoft.com/en-us/windows/win32/api/DXGI1_2/ne-dxgi1_2-dxgi_scaling) na criação da swap chain. Para bypass "puro" (DirectFlip nível 1) queremos `DXGI_SCALING_NONE` + buffers do tamanho exato do monitor.

### 2.4 Fullscreen exclusive vs borderless — os dois caminhos de posse

- **Borderless fullscreen (recomendado):** janela sem borda cobrindo 100% do monitor + swap chain flip. Em Win10/11, isso aciona DirectFlip/Independent Flip → bypass efetivo do DWM. Alt-tab é mais rápido, e a transição para composto é suave. É o que a Microsoft empurra: *"you may want to reconsider whether your application actually needs a fullscreen exclusive mode."*
- **Fullscreen exclusive clássico:** [`IDXGISwapChain::SetFullscreenState(TRUE)`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate) ainda existe. Por baixo, isso faz o runtime tomar **posse exclusiva do VidPnSource** via `D3DKMTSetVidPnSourceOwner(EXCLUSIVE)` (§3.2). No Win11, muito fullscreen exclusive DX10/11 antigo é convertido em flip ("Windowed/Fullscreen Optimizations") — o efeito de bypass é o mesmo, mas a posse pode não ser "hard exclusive". **Confirmar na build alvo com PresentMon.**

### 2.5 Waitable swap chain e latência (o loop de frame do `dispd`)

Fontes: [Reduce latency with DXGI 1.3 swap chains](https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains), [`GetFrameLatencyWaitableObject`](https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject).

- Criar a swap chain com **`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`**.
- `IDXGISwapChain2::SetMaximumFrameLatency(n)` — quantos frames a swap chain pode enfileirar (default 1 p/ waitable = menor latência; 2 p/ mais paralelismo CPU-GPU). **Chamar antes** de `GetFrameLatencyWaitableObject`.
- `GetFrameLatencyWaitableObject()` → HANDLE; o loop faz `WaitForSingleObjectEx(handle, ...)` no topo de cada frame → renderiza → `Present`. Isso sincroniza com o consumo do frame anterior. Em Independent Flip, chega a **1 frame de latência**.
- **`DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING`** (checar via `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`): present com `SyncInterval=0` + flag `DXGI_PRESENT_ALLOW_TEARING` → latência ainda menor que o waitable, até em janela com MPO. Necessário para present *unthrottled* em flip model.

### 2.6 Present statistics (medir glitch/vsync)

`IDXGISwapChain::GetFrameStatistics` → `DXGI_FRAME_STATISTICS` (`PresentCount`, `PresentRefreshCount`, `SyncRefreshCount`, `SyncQPCTime`); `GetLastPresentCount`. Em flip model isso funciona **em windowed também** (em blt windowed é tudo zero). É a base para detectar frame drops e ressincronizar. `DXGI_PRESENT_RESTART` descarta a fila de presents.

---

## 3. D3DKMT / a interface de kernel do WDDM (o caminho mais baixo de present)

Este é o substrato por baixo do DXGI. `dispd` M4 fala aqui direto.

### 3.1 Os gdi32 thunks — como DXGI chega ao kernel

Da [WDDM Architecture](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture):

> *"`gdi32.dll` is a user-mode library that a D3D runtime or a partner graphics client links against. A runtime or client calls a `gdi32` **"thunk"** that routes the call to the appropriate kernel-mode function in the DirectX kernel subsystem (`Dxgkrnl`)."*

Ou seja: as funções **`D3DKMT*`** são exportadas por **`gdi32.dll`** (lib `Gdi32.lib`, dll de forward `api-ms-win-dx-d3dkmt-l1-1-*.dll`) e são thunks finos que entram no `dxgkrnl.sys` via syscall. **DXGI usa exatamente essas thunks internamente** para present, fullscreen exclusive, e enumeração de adapter. Quem quer o caminho mais baixo chama `D3DKMT*` direto — são user-mode, documentadas (na seção de drivers), e estáveis o suficiente (usadas por todo runtime gráfico).

Pontos de entrada úteis para `dispd`:
- `D3DKMTOpenAdapterFromLuid` / `D3DKMTOpenAdapterFromGdiDisplayName` / `D3DKMTEnumAdapters2` — obter `hAdapter`.
- `D3DKMTCreateDevice` — `hDevice`.
- `D3DKMTSetVidPnSourceOwner` — **posse da saída** (§3.2).
- `D3DKMTPresent` — **present** (§3.3).
- `D3DKMTGetSharedPrimaryHandle` / `D3DKMTCreateAllocation` — primary/allocations.
- `D3DKMTWaitForVerticalBlankEvent` / `D3DKMTGetScanLine` — sincronização de vblank sem swap chain.

### 3.2 `D3DKMTSetVidPnSourceOwner` — posse do VidPnSource (quem manda no scanout)

Fonte: [`D3DKMTSetVidPnSourceOwner`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsetvidpnsourceowner) + enum [`D3DKMT_VIDPNSOURCEOWNER_TYPE`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_vidpnsourceowner_type).

> *"Sets and releases the video present source in the path of a video present network (VidPN) topology that owns the VidPN."*

Um **VidPnSource** é uma saída de vídeo (uma "fonte" que alimenta um monitor). Tomar posse é o que dá direito de escrever no primary/scanout daquela saída.

Enum `D3DKMT_VIDPNSOURCEOWNER_TYPE`:
| Valor | Significado |
|---|---|
| `D3DKMT_VIDPNSOURCEOWNER_UNOWNED` | não possuído |
| `D3DKMT_VIDPNSOURCEOWNER_SHARED` | compartilhado (composição normal; o DWM opera assim) |
| `D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE` | **exclusivo** (fullscreen exclusive D3D usa este) |
| `D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVEGDI` | **exclusivo GDI** (o dono é o caminho GDI — usado quando GDI escreve direto no primary, ex. sem DWM) |
| `D3DKMT_VIDPNSOURCEOWNER_EMULATED` | emulado |

API: preencher `D3DKMT_SETVIDPNSOURCEOWNER` com arrays `pType[]`, `pVidPnSourceId[]`, `VidPnSourceCount`. Para **liberar**: `pType=NULL`, `pVidPnSourceId=NULL`, `VidPnSourceCount=0`. Library `Gdi32.lib`, DLL `Gdi32.dll`.

Retornos que importam: `STATUS_GRAPHICS_VIDPN_SOURCE_IN_USE` (já é dono outro cliente DMM — ex. o DWM não largou), `STATUS_DEVICE_REMOVED` (adapter parou/reset).

**Chave para o `dispd`:** o par `EXCLUSIVE`/`EXCLUSIVEGDI` é *literalmente* como o Windows separa "app fullscreen D3D dona da saída" de "GDI dono da saída". `dispd` M4 pode pegar `EXCLUSIVE` do VidPnSource do monitor, tornando-se dono do scanout no lugar do DWM — sem runtime DXGI. **Risco:** se o processo morre com a saída presa, a sessão pode congelar até o dxgkrnl recuperar; testar recuperação em VM (já listado em `nt-dwm-compositor.md` §risco 4).

### 3.3 `D3DKMTPresent` + o present history token (o "modelo" do present)

Fontes: [`D3DKMTPresent`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtpresent), [`D3DKMT_PRESENT`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_present), [`D3DKMT_PRESENT_MODEL`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_present_model).

> *"`D3DKMTPresent` submits a present command to the Microsoft DirectX graphics kernel subsystem (`Dxgkrnl.sys`)."*

Campos-chave de `D3DKMT_PRESENT`:
- `hDevice`/`hContext` — device/contexto (union por compat com D3D10).
- `hWindow` — HWND alvo do bitblt; **`NULL` = desktop window**. Só obrigatório se flag `Blt`/`ColorFill`.
- `VidPnSourceId` — se `RestrictVidPnSource` setado, restringe a qual saída o present vai (com `hWindow==NULL` = fullscreen bitblt para aquela saída).
- `hSource`/`hDestination` — allocations de origem/destino (blt entre primaries).
- `FlipInterval` (`D3DDDI_FLIPINTERVAL_TYPE`) — flip após 0/1/2/3/4 vsyncs.
- `Flags` (`D3DKMT_PRESENTFLAGS`) — `Blt`, `Flip`, `ColorFill`, `SrcColorKey`, `DstColorKey`, `SrcRectValid`, `DstRectValid`, `RestrictVidPnSource`, etc.
- `PresentHistoryToken` (`D3DKMT_PRESENTHISTORYTOKEN`, Win7+) — **identifica o modelo do present** (crucial).
- `PresentLimitSemaphore`, `pPresentRegions` (dirty/move regions, Win8+), `Duration`, `bOptimizeForComposition`.

O **`D3DKMT_PRESENT_MODEL`** (dentro do history token) diz *como* o present é composto:
| Valor | Uso |
|---|---|
| `D3DKMT_PM_UNINITIALIZED` (0) | — |
| `D3DKMT_PM_REDIRECTED_GDI` (1) | present GDI redirecionado (janela GDI sob DWM) |
| `D3DKMT_PM_REDIRECTED_FLIP` (2) | **flip model** redirecionado ao DWM (swap chain flip de app janelada) |
| `D3DKMT_PM_REDIRECTED_BLT` (3) | **blt model** redirecionado ao DWM (o que `DwmDxGetWindowSharedSurface` usa, ver §1.3) |
| `D3DKMT_PM_REDIRECTED_VISTABLT` (4) | blt estilo Vista |
| `D3DKMT_PM_SCREENCAPTUREFENCE` (5) | fence de captura |
| `D3DKMT_PM_REDIRECTED_GDI_SYSMEM` (6) | GDI em sysmem |
| `D3DKMT_PM_REDIRECTED_COMPOSITION` (7) | composição (DirectComposition) |

Para o dono **exclusivo** da saída (nosso caso M4 fullscreen), o present é um **flip direto** ao scanout (não "redirected"), programado via o caminho de `SetVidPnSourceAddress` do KMD (§3.4) — não passa pelo DWM.

### 3.4 dxgkrnl: scheduling e scanout (como o flip vira foton)

Da [WDDM Architecture]: `Dxgkrnl` = **`dxgkrnl.sys`** (processa as `D3DKMT*`, modos, power, virtualização) + **`dxgmms2.sys`** (implementa **VidMm** = video memory manager e **VidSch** = GPU scheduler, WDDM 2.0+). O KMD (miniport) fala com o hardware.

Sequência de um present até o painel (caminho flip/scanout):
1. `D3DKMTPresent` (ou o UMD via DDI `pfnPresent`) enfileira o present no contexto da GPU.
2. **VidSch** agenda o pacote; **VidMm** garante que as allocations (back buffer/primary) estão residentes em vidmem.
3. Quando é hora de exibir, o dxgkrnl chama o KMD **[`DxgkDdiSetVidPnSourceAddress`](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress)** (ou `...WithMultiPlaneOverlay3`) para apontar o **endereço do primary** que a saída deve escanear — **isto é o "flip"**: trocar qual allocation o display controller lê.
4. O flip é programado para efetivar num **VSync** (respeitando `FlipOnNextVSync`/`FlipImmediate`/`FlipImmediateNoTearing`). O display controller levanta um **VSync interrupt**; o KMD reporta via `DXGKARGCB_NOTIFY_INTERRUPT_DATA` (ex. `DXGK_INTERRUPT_CRTC_VSYNC_WITH_MULTIPLANE_OVERLAY3`) com os `PresentId` completados.
5. O DAC/transmissor (HDMI/DP) faz o scanout do primary → **painel**.

**Hardware flip queue (WDDM 3.0 / Win11)** — [Hardware Flip Queue](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue): permite enfileirar **múltiplos frames futuros** no display controller com `TargetFlipTime` (em unidades de `KeQueryPerformanceCounter`/QPC), deixando CPU/GPU dormirem. KMD anuncia `MaxHwQueuedFlips>1` em `DXGK_DRIVERCAPS`; submissão via `DXGKARG_SETVIDPNSOURCEADDRESSWITHMULTIPLANEOVERLAY3`; VSync interrupt controlado por `DxgkDdiSetInterruptTargetPresentId` (present-id alvo p/ acordar). Isso é **do lado do driver** — `dispd` não implementa driver, mas entender que "o present vira `SetVidPnSourceAddress(TargetFlipTime)` no VSync" explica por que a nossa latência real depende do vsync do painel e do modo (composto vs independent flip).

**Interlocked flip** (multi-plano no mesmo VSync), **cancel** (`DxgkDdiCancelFlips` em full-screen transition / app exit) e **present statistics** (flip queue log circular) são detalhes do KMD — relevantes só se um dia formos ao nível de driver. Para `dispd` user-mode, o que importa: **o present é assíncrono e casado ao vsync**; medir com present statistics/PresentMon.

---

## 4. win32k / GDI — como o BitBlt chega (ou não) à tela

Este é o backend **M1** do `dispd` e o entendimento de por que ele funciona no bootstrap.

### 4.1 O modelo GDI: DC → BitBlt → surface

GDI clássico desenha em um **Device Context (DC)**. Uma janela dá um DC via `GetDC(hwnd)`/`BeginPaint`; um DC de tela via `GetDC(NULL)`/`CreateDC("DISPLAY",...)`. [`BitBlt`](https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt) copia um retângulo de um DC de origem (tipicamente um DC de memória com um DIB/bitmap) para o DC de destino. Este é o "desenhar pixels numa janela" mais direto que existe no Windows — e o mais fácil de implementar primeiro.

### 4.2 Full Windows (com DWM): BitBlt → CDD → redirection → DWM

Com DWM ligado (Win8+ sempre, ver `nt-dwm-compositor.md` §2), o `BitBlt` de uma app **não vai ao scanout**. Ele é interceptado pelo win32k e roteado ao **CDD** (`cdd.dll`), que rasteriza para a **redirection surface** da janela (§1.3); o **DWM** lê essa surface e compõe. Logo, no Windows normal, o resultado do `dispd`-via-GDI apareceria como uma janela *composta pelo DWM* — o que é ótimo se quisermos ser "só mais uma app", mas **não** nos dá o scanout. Por isso GDI **não** é o caminho para `dispd` "dono da tela" no Windows normal.

### 4.3 WinPE / boot (sem DWM): BitBlt → CDD/shared primary → scanout direto

**Por que o GDI-clássico está vivo no WinPE:** o WinPE é um ambiente Windows mínimo cuja shell (`winpeshl`) **não inicia o Desktop Window Manager** — não há `dwm.exe`, não há tema/Aero, não há composição. O Setup do Windows (uma app GDI/Win32) roda ali e desenha direto. Sem DWM, o caminho GDI é o clássico: win32k/CDD escreve na **shared primary surface** (o framebuffer que está sendo escaneado), e o display miniport a exibe. A HLK confirma o par de operações que caracteriza esse modo — [WDDM RotateBlt Window GDI](https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/ee64b2e6-55e0-468e-bf19-274281e0fa0f): *"Blts from a CDD shadow surface to a shared primary … and blts from a shared primary to a CDD shadow surface"* — ou seja, com composição **off**, o CDD faz blt do shadow **para o primary compartilhado** (que é o scanout). O dono do VidPnSource nesse modo é `EXCLUSIVEGDI` (§3.2).

Detalhe operacional do WinPE ([WinPE add drivers / resolution](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-8.1-and-8/dn613857(v=win.10))): resolução default **800×600** a menos que `unattend.xml`+`wpeinit` mudem; driver é tipicamente **BasicDisplay** (§5.3) se não houver IHV driver.

**Nota importante:** mesmo no WinPE, o stack por baixo é **WDDM** (não XDDM — XDDM foi removido no Win8, ver `nt-dwm-compositor.md` §3c). O que muda é apenas: **não há composição/DWM**, então o CDD (que é a face GDI do WDDM) blita para o primary que o miniport escaneia. Isto é o que valida o M1 do `dispd` no **ambiente de boot/instalação do NTUnix**, não num Windows 11 desktop com DWM vivo.

### 4.4 GDI e o flip model não se misturam

A doc de flip é explícita: *"If you have legacy components that use GDI to write to an HWND directly, use the bitblt model"*, e *"When you use the flip model, only Direct3D content in flip model swap chains … are visible. The runtime ignores all other bitblt model Direct3D or GDI content updates."* → No `dispd`, **M1 (GDI) e M3 (flip) são backends mutuamente exclusivos por surface**; não desenhar GDI numa janela que tem swap chain flip.

---

## 5. WDDM overview — split de driver e o caminho de software (o caso QEMU sem GPU driver)

### 5.1 UMD / KMD split

Da [WDDM Architecture]:
- **Direct3D runtime** (user-mode) — API p/ apps; gerencia interação app↔UMD↔`gdi32.dll`.
- **UMD (user-mode display driver)** — DLL do fornecedor, carregada pelo runtime; traduz D3D DDI em comandos.
- **`gdi32.dll` thunks** → **`Dxgkrnl`** (kernel).
- **`Dxgkrnl`** = `dxgkrnl.sys` (core: `D3DKMT*`, modos, power) + `dxgmms2.sys` (**VidMm** + **VidSch**).
- **KMD (kernel-mode display miniport)** — do fornecedor; fala com o hardware e com o `Dxgkrnl` via as DDIs `DxgkDdi*`.
- **Win32 GDI / `Win32k.sys`** — legado, ainda usado.

Um IHV **precisa** fornecer UMD **e** KMD. Isolamento: crash do UMD não derruba o kernel; timeout de GPU (2s default) dispara **TDR** via `DxgkDdiResetFromTimeout`.

### 5.2 O caminho de software: WARP

[WARP](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp) (Windows Advanced Rasterization Platform) é um **rasterizador de software de alta performance e totalmente conformante** com D3D. É usado quando **não há GPU/driver adequado**, ou para offload à CPU. A partir do Win8, existe como adapter enumerável no DXGI (fallback transparente).

### 5.3 BasicDisplay + BasicRender (o nosso caso na VM QEMU)

Fonte primária: [Microsoft Basic Display Driver](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver).

- **`BasicDisplay.sys`** é o driver in-box genérico que **substitui os drivers XDDM VGA Save / VGA PnP**. Carregado em safe mode, no setup, em transição de driver, ou quando **não há driver IHV**.
- Propósito: *"enable Windows to write to the display controller's **linear frame buffer**."*
- **É um driver WDDM** (não XDDM): *"the … functionality (specifically, features like reboot-less updates, dynamic start and stop, …) that are provided by the **WDDM driver model**."* Mas *"works on both WDDM and legacy XDDM hardware"* e suporta **UEFI GOP** (herda o linear framebuffer setado no boot; em UEFI, **sem mudança de modo/resolução**).
- **Compatível com Desktop Composition:** *"BasicDisplay helps to enable a consistent … experience because it's compatible with DirectX APIs and technologies such as the **Desktop Composition**."* → **O DWM roda sobre BasicDisplay** (num desktop normal). Só que **sem aceleração 3D de hardware** — o render é **WARP na CPU**.
- **Sempre pareado com `BasicRender`:** *"BasicDisplay is always used with BasicRender, which is the system-supplied module that exposes the functionality of **WARP** from an adapter in the kernel."* Win11: ambos rodam do DriverStore.

**Consequência para o `dispd` na VM QEMU (sem driver IHV):**
- Se rodarmos um Windows desktop cru na QEMU sem virtio-gpu/driver, o adapter é **BasicDisplay + BasicRender(WARP)**. Present/scanout funcionam (o miniport escaneia o linear framebuffer), mas 3D é **CPU (WARP)** → lento, mas **funciona**.
- **M1 (GDI BitBlt)** funciona perfeitamente sobre BasicDisplay (é literalmente escrever no linear framebuffer via CDD) — por isso é o backend certo para "primeiro pixel" na VM.
- **M3 (DXGI flip)** funciona sobre BasicDisplay+WARP, mas **DirectFlip/Independent Flip/MPO tendem a NÃO engatar** (sem hardware de scanout/overlay dedicado) → cai em composição via WARP. Ou seja, o "bypass do DWM" pode não acontecer na VM sem GPU. **Confirmar com PresentMon.** Para o bypass real, precisamos de um driver com scanout de verdade (virtio-gpu com WDDM, ou GPU passthrough).
- **M4 (D3DKMT EXCLUSIVE)** sobre BasicDisplay: `SetVidPnSourceOwner(EXCLUSIVE)` + present flip deve dar posse do linear framebuffer; degradação graciosa esperada, mas **testar** (é o risco 7 de `nt-dwm-compositor.md`).

**Recomendação de VM:** para desenvolver M3/M4 com bypass real, usar **virtio-gpu com driver WDDM** (ou GPU passthrough); para M1 e para testar degradação, BasicDisplay basta.

---

## 6. Input — Raw Input pipeline (RIM) e os class drivers

Quando `dispd` for dono da tela (M3/M4 fullscreen), o input não pode depender do foco de janela clássico. O caminho é **Raw Input**.

### 6.1 O pipeline de kernel (RIM / Raw Input Thread)

- Hardware HID (USB/Bluetooth) → **`hidclass.sys`** (a "cola" entre `KBDHID.sys`/`MOUHID.sys` e os transportes) → **class drivers** **`kbdclass.sys`** / **`mouclass.sys`**.
- Os class drivers impõem um protocolo que divide a stack em "upper" (independente de hardware) e "lower" (dependente); a lower entrega dados via **class service callback**. Fonte: [Keyboard and mouse HID client drivers](https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers).
- No topo, o **Raw Input Thread (RIT)** do **win32k** (`win32k!RawInputThread`) envia read IRPs aos class drivers pedindo até N pacotes; entende dois tipos: **`KEYBOARD_INPUT_DATA`** e **`MOUSE_INPUT_DATA`**. `win32k!ProcessMouseInput` aplica o movimento e enfileira em `win32k!gMouseEventQueue`. (Esse é o **Raw Input Manager / RIM** internamente.)
- A partir daí, o win32k gera **mensagens legadas** (`WM_MOUSEMOVE`, `WM_KEYDOWN`, …) **e/ou** entrega **Raw Input** (`WM_INPUT`) aos registrantes.

### 6.2 Raw Input API (o que `dispd` usa)

Fontes: [Raw Input Overview](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input), [Using Raw Input](https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input), [RegisterRawInputDevices](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices).

- **Registro:** `RegisterRawInputDevices(RAWINPUTDEVICE[])`. Cada entrada = Top Level Collection por **UsagePage/Usage**: teclado = `UsagePage=0x01` (`HID_USAGE_PAGE_GENERIC`), `Usage=0x06`; mouse = `0x01`/`0x02`.
- **Flags-chave:**
  - `RIDEV_NOLEGACY` — **suprime** as mensagens legadas (`WM_KEYDOWN`, `WM_LBUTTONDOWN`, …) para aquele device. Isto é o que desacopla `dispd` do "message path" clássico — recebemos só HID cru.
  - `RIDEV_INPUTSINK` — recebe input **mesmo em background** (não precisa ser foreground). Útil quando `dispd` é dono do scanout mas não "a janela em foco".
  - `RIDEV_DEVNOTIFY` — notifica hot-plug de device (`WM_INPUT_DEVICE_CHANGE`).
- **Leitura:**
  - **Padrão:** `GetMessage` → `WM_INPUT` → `GetRawInputData(HRAWINPUT, ...)` → `RAWINPUT` (um por vez).
  - **Buffered (recomendado p/ mouse 1000 Hz):** `GetRawInputBuffer` drena um **array** de `RAWINPUT` de uma vez, evitando 1 mensagem por evento. Essencial p/ baixa latência.
- **Precisa de um HWND** (mesmo invisível/fullscreen) para receber `WM_INPUT` — trivial no cenário fullscreen do `dispd`.

### 6.3 Diferença vs o message path normal

No path normal, o win32k roteia input **para a janela em foco** como mensagens sintetizadas (com aceleração/ballistics de mouse, layout de teclado, dead keys já aplicados). No Raw Input, recebemos os **pacotes HID crus** (deltas de mouse sem aceleração, scancodes), independentes de foco/hit-testing — que é exatamente o que um compositor/WM próprio quer: nós fazemos nosso próprio hit-testing, aceleração e roteamento para *nossas* janelas nativas. `RIDEV_NOLEGACY` garante que o win32k não fique também gerando o path legado por baixo.

**Mais baixo (fase "user space independente"):** falar direto com `\Device\RawInputManager` ou com `kbdclass`/`mouclass` (IOCTLs, class service callback) é undocumented e desnecessário no início — Raw Input buffered já entrega latência de jogo. Guardar para quando `dispd` largar totalmente o loop de mensagens Win32.

---

## 7. Roadmap concreto de present backend para o `dispd` (GDI → DXGI flip → D3DKMT)

Cada estágio é um backend plugável atrás de uma interface `present(surface)` do `dispd`. APIs exatas por estágio:

### M1 — GDI BitBlt (bootstrap / WinPE / console / VM sem GPU)
**Onde funciona como "dono da tela":** ambiente **sem DWM** (WinPE, sessão de boot/instalação do NTUnix, safe mode). Em Windows desktop normal, aparece como janela composta pelo DWM (útil como "app", não como display server).
**APIs:**
- Surface: `CreateDIBSection` (back buffer em memória, 32bpp obrigatório) + `CreateCompatibleDC`.
- Tela: `GetDC(NULL)` ou `CreateDC(L"DISPLAY", NULL, NULL, NULL)` para o primary; ou `BeginPaint`/`GetDC(hwnd)` p/ janela.
- Present: `BitBlt(hdcScreen, x, y, w, h, hdcMem, 0, 0, SRCCOPY)` (ou `StretchBlt`).
- VSync (opcional): `D3DKMTWaitForVerticalBlankEvent` p/ evitar tearing.
- Modo/resolução: `ChangeDisplaySettingsEx` (fora do UEFI-locked BasicDisplay).
**Uso:** primeiro pixel, TTY/console gráfico, fallback universal (funciona sobre BasicDisplay/WARP).

### M3 — DXGI flip-model (backend gráfico principal)
**Onde bypassa o DWM:** borderless cobrindo 100% do monitor + swap chain flip → DirectFlip/Independent Flip (precisa de scanout de hardware real; em BasicDisplay/WARP pode cair em composição).
**APIs:**
- Device: `D3D11CreateDevice` (ou D3D12) + `CreateDXGIFactory2`.
- Swap chain: `IDXGIFactory2::CreateSwapChainForHwnd` com `DXGI_SWAP_CHAIN_DESC1`:
  - `SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD` (ou `FLIP_SEQUENTIAL` se precisar preservar).
  - `BufferCount ≥ 2` (usar 2–3), `Format = B8G8R8A8_UNORM`/`R8G8B8A8_UNORM`, `SampleDesc={1,0}`.
  - `Scaling = DXGI_SCALING_NONE`, buffers == tamanho do monitor.
  - `Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` (+ `ALLOW_TEARING` se suportado).
- Latência: `IDXGISwapChain2::SetMaximumFrameLatency(1)` → `GetFrameLatencyWaitableObject` → `WaitForSingleObjectEx` no loop.
- Present: `IDXGISwapChain1::Present1(syncInterval, flags)` (`flags |= DXGI_PRESENT_ALLOW_TEARING` com `syncInterval=0`).
- Capacidade: `IDXGIOutput6::CheckHardwareCompositionSupport`, `IDXGIFactory5::CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING)`.
- Fullscreen (opcional): `SetFullscreenState(TRUE)` + `ResizeBuffers` (preferir borderless).
- Medição: `GetFrameStatistics` / PresentMon p/ confirmar Independent Flip.
**Uso:** desktop nativo do NTUnix acelerado, com nosso WM/compositor desenhando na swap chain. **Este é o M3 alvo.**

### M4 — D3DKMT direto (posse do scanout, sem runtime DXGI)
**Onde:** máximo controle/menor latência; `dispd` vira dono do VidPnSource.
**APIs:**
- Adapter/device: `D3DKMTOpenAdapterFromGdiDisplayName` / `D3DKMTEnumAdapters2` → `D3DKMTCreateDevice`.
- **Posse:** `D3DKMTSetVidPnSourceOwner` com `pType = D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE` (ou `EXCLUSIVEGDI` p/ caminho GDI) para o `VidPnSourceId` do monitor.
- Allocations/primary: `D3DKMTCreateAllocation` / `D3DKMTGetSharedPrimaryHandle`.
- Present: `D3DKMTPresent` com `D3DKMT_PRESENT` (Flip flag, `FlipInterval`, `PresentHistoryToken`); ou `D3DKMTPresentMultiPlaneOverlay*` p/ MPO.
- VSync: `D3DKMTWaitForVerticalBlankEvent`, `D3DKMTGetScanLine`.
- Liberar posse: `D3DKMTSetVidPnSourceOwner(pType=NULL, count=0)` — **imprescindível** no shutdown/crash-handler p/ não brickar a sessão.
**Uso:** otimização/independência depois que M3 estiver de pé. **Risco:** posse presa se o processo morre; testar recuperação em VM.

**Ordem recomendada:** M1 (já) → M3 (backend gráfico) → M4 (independência). Input: **Raw Input buffered** (`RIDEV_NOLEGACY|RIDEV_INPUTSINK` + `GetRawInputBuffer`) desde o M3.

---

## 8. Insights mais profundos (o que não é óbvio)

1. **"Ser dono da tela" ≠ "ser o compositor do Windows".** Há dois eixos independentes: (a) posse do **VidPnSource/scanout** (`SetVidPnSourceOwner`, flip chain do monitor) e (b) o **object database de composição** win32k↔dwmcore. `dispd` mira **(a)** e ignora **(b)**. Confundir os dois é o que faz projetos tentarem (inutilmente) reimplementar o dwmcore.

2. **O flip model foi projetado pela Microsoft para *bypassar a própria composição*.** Independent Flip é "eficiência de fullscreen exclusive numa janela borderless". Isso é o presente perfeito para o `dispd`: o caminho sancionado, público e estável de tomar o scanout é *literalmente uma swap chain flip cobrindo o monitor* — não precisamos de nada undocumented para o M3.

3. **`EXCLUSIVE` vs `EXCLUSIVEGDI` no enum de VidPnSource owner é o mapa do Windows entre "app fullscreen D3D" e "GDI dono da tela".** Isso conecta M1 (GDI, `EXCLUSIVEGDI`, modo sem-DWM/WinPE) e M4 (D3D exclusivo, `EXCLUSIVE`) como **duas posses do mesmo VidPnSource** — não são caminhos diferentes, são o mesmo mecanismo com donos diferentes.

4. **Sob WDDM, "GDI sem DWM" não é XDDM.** O CDD é a face GDI *do WDDM*. No WinPE, GDI chega ao scanout porque **não há DWM compondo**, não porque exista um caminho XDDM. Isso é sutil e importante: o M1 do `dispd` roda sobre WDDM/BasicDisplay, só que aproveita a ausência de composição no ambiente de boot.

5. **BasicDisplay+WARP salva a VM QEMU, mas mata o bypass.** M1 e M4-blt funcionam; M3 provavelmente **não** engata Independent Flip (sem scanout/overlay de hardware) e cai em composição via WARP na CPU. Para validar o bypass real do M3, precisa de virtio-gpu-WDDM ou passthrough. Isso muda a estratégia de teste: **M1 na VM crua; M3 com GPU/virtio real.**

6. **O present é assíncrono e casado ao vsync no nível do KMD (`SetVidPnSourceAddress` no VSync).** Nossa latência real (mesmo em D3DKMT) é ditada pelo vsync do painel e pelo modo (composto vs independent). Não dá para "burlar" isso em user-mode; o que dá é escolher o modo (waitable + allow-tearing) e medir (PresentMon/present statistics).

7. **Raw Input com `RIDEV_NOLEGACY` é o "desligar o roteamento de mensagens do win32k" sancionado.** Recebemos HID cru (deltas sem aceleração, scancodes) e o win32k para de gerar o path legado — exatamente o que um WM próprio precisa. Buffered (`GetRawInputBuffer`) é obrigatório p/ mouse de alta taxa.

8. **DWM é obrigatório e não-desligável desde o Win8** (já em `nt-dwm-compositor.md` §2) — então a estratégia nunca é "desligar o DWM de um sistema vivo", e sim **não iniciar o DWM na sessão do NTUnix** e prover nós mesmos o dono do scanout. No ambiente de boot/instalação (sem DWM) isso é o estado natural; numa sessão desktop, é tomar posse via flip/VidPnSource.

---

## 9. Fontes

Primárias (Microsoft Learn / Windows Driver Docs):
- DirectComposition — Architecture and components (dcomp/dwmcore/win32k, per-session): https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components
- DirectComposition — Bitmap objects (redirection/composition/swapchain como content): https://learn.microsoft.com/en-us/windows/win32/directcomp/bitmap-surfaces
- DwmDxGetWindowSharedSurface (produtora, driver-only, ordinal 100): https://learn.microsoft.com/en-us/windows/win32/dwm/dwmdxgetwindowsharedsurface
- Desktop Duplication API (única leitura sancionada do composto): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- DXGI flip model (blt vs flip, sharing com DWM): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/dxgi-flip-model
- For best performance, use DXGI flip model (DirectFlip/Independent Flip/MPO/waitable/allow-tearing): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- DXGI flip model dev blog: https://devblogs.microsoft.com/directx/dxgi-flip-model/
- DXGI_SWAP_EFFECT enum: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/ne-dxgi-dxgi_swap_effect
- IDXGISwapChain2::GetFrameLatencyWaitableObject: https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_3/nf-dxgi1_3-idxgiswapchain2-getframelatencywaitableobject
- Reduce latency with DXGI 1.3 swap chains: https://learn.microsoft.com/en-us/windows/uwp/gaming/reduce-latency-with-dxgi-1-3-swap-chains
- IDXGISwapChain::SetFullscreenState: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate
- D3DKMTSetVidPnSourceOwner: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtsetvidpnsourceowner
- D3DKMT_VIDPNSOURCEOWNER_TYPE enum (UNOWNED/SHARED/EXCLUSIVE/EXCLUSIVEGDI/EMULATED): https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_vidpnsourceowner_type
- D3DKMTPresent: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtpresent
- D3DKMT_PRESENT struct: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_present
- D3DKMT_PRESENT_MODEL / PresentHistoryToken (REDIRECTED_FLIP/BLT/GDI/COMPOSITION): https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ne-d3dkmthk-_d3dkmt_present_model
- WDDM Architecture (UMD/KMD, gdi32 thunks, Dxgkrnl/VidMm/VidSch): https://learn.microsoft.com/en-us/windows-hardware/drivers/display/windows-vista-and-later-display-driver-model-architecture
- Hardware Flip Queue (SetVidPnSourceAddress, TargetFlipTime, VSync interrupt, WDDM 3.0): https://learn.microsoft.com/en-us/windows-hardware/drivers/display/hardware-flip-queue
- DxgkDdiSetVidPnSourceAddress: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmddi/nc-d3dkmddi-dxgkddi_setvidpnsourceaddress
- Microsoft Basic Display Driver (BasicDisplay + BasicRender/WARP, WDDM, linear framebuffer, Desktop Composition compat): https://learn.microsoft.com/en-us/windows-hardware/drivers/display/microsoft-basic-display-driver
- Windows Advanced Rasterization Platform (WARP): https://learn.microsoft.com/en-us/windows/win32/direct3darticles/directx-warp
- Raw Input Overview: https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
- Using Raw Input (GetRawInputData/GetRawInputBuffer, flags): https://learn.microsoft.com/en-us/windows/win32/inputdev/using-raw-input
- RegisterRawInputDevices (RIDEV_NOLEGACY/INPUTSINK/DEVNOTIFY): https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerrawinputdevices
- Keyboard and mouse HID client drivers (kbdclass/mouclass/hidclass): https://learn.microsoft.com/en-us/windows-hardware/drivers/hid/keyboard-and-mouse-hid-client-drivers
- BitBlt: https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-bitblt
- WDDM RotateBlt Window GDI (CDD shadow ↔ shared primary, composição off): https://learn.microsoft.com/en-us/windows-hardware/test/hlk/testref/ee64b2e6-55e0-468e-bf19-274281e0fa0f
- WinPE add drivers / resolução (800x600, wpeinit): https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-8.1-and-8/dn613857(v=win.10)

Secundárias / referência técnica:
- Wikipedia — Desktop Window Manager (GDI→CDD→redirection, shared primary): https://en.wikipedia.org/wiki/Desktop_Window_Manager
- win32k input pipeline (RawInputThread/ProcessMouseInput) — MouHidInputHook README: https://github.com/changeofpace/MouHidInputHook

Repo (cross-reference):
- `docs/pesquisa/nt-dwm-compositor.md` — a "verdade honesta" sobre por que NÃO substituir o DWM; caminhos 3a–3d; riscos p/ VM.
- `docs/VISAO.md` §17 (Ambiente gráfico), §18 (DWM e compatibilidade gráfica).
