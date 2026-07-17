# NT / DWM / Compositor próprio — verdade técnica para o NTUnix

> Pesquisa técnica para responder: **é viável tirar o DWM e fazer o compositor próprio do NTUnix "já de cara"?**
> Foco: Windows 11 / WDDM moderno. Sem otimismo. Cada afirmação forte tem fonte.
> Complementa `docs/VISAO.md` §17 (Ambiente gráfico) e §18 (DWM e compatibilidade gráfica).

---

## 0. Veredito em uma frase

**"Fazer o próprio DWM de cara" no sentido de *substituir* o dwm.exe e compor as janelas Win32 existentes NÃO é realista agora** — é reescrever o stack gráfico e depender de APIs internas do dwmcore que a Microsoft nunca expôs. **Mas "mostrar uma janela NATIVA do NTUnix na tela com um compositor próprio" É realista já**, por um caminho diferente: tomar posse da saída de vídeo com uma *swap chain flip-model* (ou VidPN exclusive) e desenhar nós mesmos, deixando o DWM/Win32 numa camada isolada por baixo. Ou seja: os caminhos **3b** e **3d** são viáveis para o *primeiro* compositor; **3a** e **3c** não são.

---

## 1. Como o Windows 11 compõe o desktop hoje

### 1.1 As peças

| Componente | Onde roda | Papel |
|---|---|---|
| `dwm.exe` | user-mode, por sessão (Session 1+) | processo do compositor; hospeda `dwmcore.dll` |
| `dwmcore.dll` | dentro do `dwm.exe` | **motor de composição** de fato (a engine do DirectComposition) |
| `dcomp.dll` | user-mode, em CADA app | API pública COM do DirectComposition (produz a árvore de visuais do app) |
| `win32kbase.sys` / `win32kfull.sys` / `win32k.sys` | kernel, por sessão | window manager (NtUser) + GDI (NtGdi) + **object database do DirectComposition** que faz marshalling dos comandos app→engine |
| `cdd.dll` (Canonical Display Driver) | kernel | rasterizador de software que recebe GDI e escreve em bitmaps na RAM para o DWM ler |
| `dxgkrnl.sys` (DirectX Graphics Kernel) | kernel | agenda GPU, aloca surfaces, faz o present real ao scanout |

Fonte da divisão de papéis (dcomp/dwmcore/win32k): [Microsoft Learn — DirectComposition, Architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components) — *"A user-mode composition engine (dwmcore.dll) that is hosted in the Desktop Window Manager (DWM) process (dwm.exe) … A kernel-mode object database (part of win32k.sys) that marshals commands from the application to the composition engine."*

Fonte da divisão do win32k em Windows 10+ (`win32kbase.sys`+`win32kfull.sys`): [Microsoft Q&A / referências geoffchappell](https://learn.microsoft.com/en-us/answers/questions/3723188/win32kbase-sys-and-win32kfull-sys-bsod) e a árvore do win32k (ntuser = window manager, ntgdi = GDI) em [geoffchappell / win32k tree](https://geob99.github.io/pages/doc/win32k-tree.html).

### 1.2 Redirection surface — o coração do problema

O DWM **não** deixa cada janela desenhar direto no scanout. Cada janela top-level ganha uma **redirection surface** (superfície de redirecionamento) fora da tela; o app desenha ali, e o dwmcore lê essas surfaces de TODAS as janelas e compõe a árvore inteira do desktop.

- **Apps GDI**: as chamadas GDI são interceptadas pelo `win32k.sys` e roteadas ao **CDD.dll** (rasterizador de software), que escreve o resultado num bitmap em RAM do tamanho da janela; o DWM lê esse bitmap. Fonte: [Wikipedia — Desktop Window Manager](https://en.wikipedia.org/wiki/Desktop_Window_Manager) e o blog do ReactOS abaixo (§4). Resumo confirmado por múltiplas fontes: *"Under DWM, GDI calls are redirected to use the Canonical Display Driver (cdd.dll), a software renderer, with a buffer equal to the size of the window allocated in system memory … which DWM can then access for compositing."*
- **Apps DirectX/WPF**: usam WDDM e compartilham a surface D3D direto com o DWM via `dxgkrnl.sys`, sem cópia de bitmap.

O "window bitmap" que o DirectComposition usa é, na real, um placeholder que a engine troca em tempo real por uma de três coisas: *a composition surface, uma DXGI swap chain, ou a redirection surface de outra janela* — ver a doc de [Bitmap objects (DirectComposition)](https://learn.microsoft.com/en-us/windows/win32/directcomp/bitmap-surfaces).

### 1.3 A API de redirection surface é privada / driver-only

Existe uma API documentada que expõe a redirection surface de uma janela: **`DwmDxGetWindowSharedSurface`** — *"Retrieves the DirectX shared surface backing a given window. This surface can be written to in order to update the contents of the window."* Depois de escrever, você chama `D3DKMTPresent` com `PresentHistoryToken.Model = D3DKMT_PM_REDIRECTED_BLT`. Fonte: [Microsoft Learn — DwmDxGetWindowSharedSurface](https://learn.microsoft.com/en-us/windows/win32/dwm/dwmdxgetwindowsharedsurface).

Três detalhes que matam qualquer plano ingênuo:

1. *"This API is intended for implementing a graphics driver or runtime. **An application may not call this method.**"*
2. *"This documentation is only valid for **Windows 7**, and this API is **not guaranteed to exist nor behave in a similar manner on other versions of Windows.**"*
3. *"This function is **not present in any header or static-link library**, and it is located at **ordinal 100 in dwmapi.dll**."*

E — crucial — essa é a face **produtora** (o runtime D3D *escreve* na surface da SUA janela para o DWM compor). **Não existe API pública para a face consumidora**: enumerar e *ler* as redirection surfaces de *todas* as janelas do sistema para compor você mesmo. Isso é 100% interno ao `dwmcore.dll`. Confirmação indireta: a única forma sancionada de obter pixels compostos de fora do DWM é a **Desktop Duplication API**, que entrega o *desktop inteiro já composto*, nunca superfícies por-janela — [Microsoft Learn — Desktop Duplication API](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).

### 1.4 Como o DWM entrega o quadro final à tela

O loop do dwmcore roda um quadro por vblank e no fim faz *"Present the frame by flipping the back and front buffers for each screen. … Each monitor is represented by a separate **full-screen flip chain**"* — [DirectComposition, Architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components). Ou seja: **o DWM detém uma flip chain fullscreen por monitor**. Quem controla essa flip chain controla o scanout. Normalmente é o DWM; em fullscreen exclusive / independent flip é o app (ver §3b). Por baixo, isso é `D3DKMTPresent` → `dxgkrnl.sys` → scanout, e a posse da saída é gerida por `D3DKMTSetVidPnSourceOwner` (§3d).

---

## 2. Dá para DESLIGAR o DWM no Windows 11?

**Não de forma útil.** A composição é obrigatória desde o Windows 8.

Fonte primária (Microsoft Compatibility Cookbook — [Desktop Window Manager is always on](https://learn.microsoft.com/en-us/windows/compatibility/desktop-window-manager-is-always-on)):

- *"In Windows 8, Desktop Window Manager (DWM) is **always ON and cannot be disabled** by end users and apps."*
- *"In Windows 8, DWM desktop composition is a **core operating system component and cannot be disabled**. With a few exceptions, desktop composition is always on; it's **started before the user logon** and remains active for the duration of a session."*
- *"**All of the options for disabling desktop composition that exist in Windows 7 are removed.**"* — incluindo o shim "Disable desktop composition" e a opção na aba Compatibilidade.
- *"Apps cannot use `DwmEnableComposition` to disable desktop composition. In order to maintain backward compatibility, a call to this API will return success; however, **desktop composition is not disabled.**"*

Confirmação na doc da própria função: [DwmEnableComposition](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmenablecomposition) — deprecada; com `DWM_EC_DISABLECOMPOSITION` em Win8+ não faz nada.

**Não existe mais o modo "Basic"/unaccelerated do Win7.** Em Win8+ a composição roda para *todos* os temas (Classic e high-contrast inclusive), e se não houver driver WDDM o Windows usa o **Microsoft Basic Display Adapter** (rasterizador de software na CPU) — mas o *DWM continua rodando* nele. A "composição básica" do Win7 (janelas sem Aero, sem DWM) simplesmente não existe. Também: cor tem que ser 32 bpp obrigatório (fonte: mesma página do Cookbook).

**E se você matar o `dwm.exe`?** Ele reinicia automaticamente (o SCM/UxSms respawna). O serviço **UxSms** ("Desktop Window Manager Session Manager") pode ser parado com `net stop UxSms` (admin), mas isso desestabiliza a GUI e ele volta. Projetos como *NoMoreDWM* existem justamente porque não há caminho suportado. Fontes: threads de comunidade ([WinClassic](https://winclassic.net/thread/159/alternative-method-disabling-dwm-windows), [NoMoreDWM](https://github.com/TK50P/NoMoreDWM)). Essas são fontes fracas (fóruns), mas todas concordam e batem com a doc oficial acima.

**Ponto que importa para o NTUnix:** matar o DWM num Windows normal é ruim porque *nada* substitui a composição — a tela congela/fica preta, já que o caminho GDI→CDD→redirection surface só chega ao scanout *através* do DWM. Mas o NTUnix *controla o user space e a sessão*: a questão não é "matar o DWM de um sistema vivo" e sim "**não iniciar o DWM e prover nós mesmos quem lê o scanout**". Isso muda o problema de "desligar" para "substituir o dono da saída de vídeo" — que é o §3.

---

## 3. Caminhos reais para um compositor próprio no NTUnix

### 3a. Substituir o dwm.exe lendo as redirection surfaces das janelas Win32

**O que seria:** escrever um `ntdwm` que faz o que o dwmcore faz — enumerar as janelas, pegar a redirection surface de cada uma (GDI e D3D), compor a árvore e apresentar.

**Viabilidade: essencialmente inviável agora.** Motivos, com fonte:

- **Não há API consumidora pública.** A leitura das redirection surfaces de *todas* as janelas é interna ao `dwmcore.dll`. A única API que toca redirection surface é `DwmDxGetWindowSharedSurface` (produtora, Win7-only, driver-only, ordinal 100, "an application may not call this method") — [ref](https://learn.microsoft.com/en-us/windows/win32/dwm/dwmdxgetwindowsharedsurface). A única leitura sancionada do resultado é a Desktop Duplication API, que dá o desktop *já composto*, não janelas separadas — [ref](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api).
- **O object database do DirectComposition vive no `win32k.sys` por sessão** e espera exatamente *um* motor de composição por sessão (o dwmcore). Fonte: [Architecture and components](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components) (*"Both the kernel-mode object database and the user-mode composition engine are instantiated once per session"*). Colocar um segundo motor implicaria reimplementar/falsificar o protocolo interno win32k↔dwmcore — engenharia reversa de contrato não-documentado e versionado a cada build do Windows.
- **É exatamente o que o ReactOS ainda não conseguiu** mesmo com todo o código NT deles (§4).

**Conclusão 3a:** só faz sentido como projeto de P&D de longo prazo (reversar dwmcore + o protocolo win32k). Não é o "primeiro compositor".

### 3b. Compositor fullscreen via DXGI/D3D (flip model) desenhando só janelas NATIVAS do NTUnix

**O que seria:** o NTUnix cria uma **swap chain flip-model** (D3D11/D3D12 + DXGI) cobrindo o monitor inteiro — em *fullscreen exclusive* (`SetFullscreenState`) ou *borderless que cobre 100% do monitor* (independent flip). Nosso compositor desenha as janelas nativas do NTUnix (nossas apps) nessa surface e apresenta. Apps Win32, se existirem, ficam com o DWM numa camada por baixo, isolada.

**Viabilidade: alta — este é o caminho realista para o primeiro compositor.** Fundamentos, com fonte:

- O flip model foi feito para *bypassar* a composição do DWM: em windowed fullscreen o *Independent Flip* *"displays the application swap chain directly on the screen … eliminates desktop composition done by the Desktop Window Manager in the same way that exclusive full-screen does"* — [Microsoft Learn — For best performance, use DXGI flip model](https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model) e o [DirectX Developer Blog](https://devblogs.microsoft.com/directx/dxgi-flip-model/).
- Em Win10/11 basta uma janela borderless cobrindo o monitor inteiro, sem escala, com swap chain flip, para bypassar o compositor (confirmação prática, fonte fraca porém consistente: [Blur Busters](https://forums.blurbusters.com/viewtopic.php?t=12139&start=10)).
- `IDXGISwapChain::SetFullscreenState` (fullscreen exclusive) ainda existe em Win11 — [ref](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate). (Nota: Win11 tende a *converter* fullscreen exclusive DX10/11 antigo em flip "Windowed Optimizations" — o efeito prático de bypass é o mesmo.)

**Prós:** usa APIs 100% públicas e estáveis (DXGI/D3D). É o mesmo caminho que todo jogo usa. Dá para ter nosso próprio window manager/compositor completo (tiled, workspaces, painel etc.) desenhando na nossa surface. Independe do DWM.

**Contras / limites honestos:**
- Enquanto nosso fullscreen está ativo, ele *cobre* tudo — as janelas Win32 (compostas pelo DWM) ficam por baixo, não visíveis, a menos que a gente as capture e desenhe (aí cai no problema §3a, ou usa Desktop Duplication para trazer o desktop Win32 inteiro como uma "textura" — grosso, mas possível).
- Alt-tab/foco: precisamos gerenciar quando cedemos a saída (ex.: para uma app Win32 fullscreen).
- Multi-monitor: uma flip chain por monitor (o DWM faz igual — [ref](https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components)).
- Não é "substituir o DWM"; é "rodar por cima/no lugar do DWM para o *nosso* conteúdo". Para o objetivo do NTUnix (desktop Unix nativo) isso é *exatamente o que se quer* de início.

### 3c. Modo GDI clássico sem composição (estilo ReactOS / Win7-basic)

**O que seria:** janelas desenham direto no framebuffer via GDI, sem composição, como no Win2000/XP/Win7-basic.

**Viabilidade: não existe mais no Windows 11.** Fonte: o modo "basic"/sem-composição foi *removido* no Win8 (§2, Compatibility Cookbook). No Win8+, GDI é sempre roteado via CDD para uma redirection surface que **só o DWM** leva ao scanout — sem DWM, o GDI não chega à tela. Além disso o **XDDM foi removido no Win8** (*"XDDM is no longer supported, and XDDM drivers do not load on Windows 8"* — [WDDM in Windows 8](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-in-windows-8)), e é justamente o XDDM que o modelo GDI-clássico do ReactOS pressupõe. Portanto **3c está fechado no Win11**; só seria "viável" se o NTUnix trocasse o driver de display por um XDDM próprio — o que é reescrever driver de kernel, não user space.

### 3d. Acesso mais baixo: WDDM/DXGI direto, D3DKMT / VidPN owner, ou IddCx

Duas sub-variantes, ambas mais baixas que o 3b:

**(i) Tomar posse da saída via D3DKMT / VidPN.** Por baixo do DXGI, a posse de um "video present source" (uma saída de vídeo) é feita por **`D3DKMTSetVidPnSourceOwner`**, com tipos como `D3DKMT_VIDPNSOURCEOWNER_EXCLUSIVE` / `EXCLUSIVEGDI` — [ref](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setvidpnsourceowner). O present é `D3DKMTPresent` → `dxgkrnl.sys` — [ref](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtpresent). Há até `D3DKMTGetSharedPrimaryHandle`. Essas são as thunks de `gdi32.dll` chamáveis de user-mode que **DXGI usa internamente para fullscreen exclusive**. É o substrato "de verdade" para um compositor NTUnix que assume o monitor. Mais poder e menos dependência do runtime DXGI, mas mais undocumented e mais fácil de brickar a sessão. Recomendado só *depois* que 3b estiver de pé, como otimização/independência.

**(ii) IddCx (Indirect Display Driver).** Cria uma **tela virtual** própria via classe `IddCx`; o Windows manda o desktop composto para o nosso driver (UMDF, roda em Session 0), e nós processamos a imagem. Fonte: [Indirect Display Driver Model Overview](https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview) (e o [IddSampleDriver](https://github.com/ge9/IddSampleDriver)). Útil como *sink* (ex.: streaming, headless, monitor virtual), **não** como jeito de *compor nossas janelas na tela física* — é o inverso do que queremos para o desktop. Guardar para casos "tela virtual/streaming", não para o compositor principal.

---

## 4. O que o ReactOS faz (referência de um NT *sem* DWM)

O ReactOS é a melhor referência viva de "NT que desenha o desktop sem DWM": ele usa **GDI + Win32k + XDDM**, com **`CDD.dll` como display driver**, e **não tem DWM** — o desktop é desenhado pelo Win32k direto. Fonte: [ReactOS — An initial investigation into WDDM](https://reactos.org/blogs/investigating-wddm/): *"CDD.dll first and foremost IS an XDDM display driver … even if it's translating from the old world Win32k to the new world WDDM stack it's still stressing Win32k out"* e *"For ReactOS to be truly compatible with WDDM, our XDDM stack must be in great shape."*

**O que dá para reaproveitar conceitualmente:**
- A arquitetura NtUser (window manager) + NtGdi (GDI) do Win32k como modelo de referência de como janelas, mensagens e objetos USER funcionam — [geoffchappell / win32k tree](https://geob99.github.io/pages/doc/win32k-tree.html).
- A prova de que **é possível ter desktop NT sem DWM** — mas **com XDDM**, não WDDM. Ou seja, o modelo ReactOS *não roda como está* no hardware/driver WDDM do Win11.

**O que NÃO dá para copiar direto:** o NTUnix roda sobre o **kernel NT real do Win11 + WDDM**, onde XDDM não carrega e GDI sem DWM não chega ao scanout (§2, §3c). Então o "jeito ReactOS" só serviria se o NTUnix também trocasse o modelo de driver — o que contraria a premissa de *preservar kernel/drivers/Win32*. Reaproveita-se a *arquitetura de window manager*, não o *caminho de apresentação*.

---

## 5. Input (mouse/teclado sem o pipeline normal)

Se o NTUnix assume a tela com um compositor próprio (3b/3d), ainda dá para pegar input por caminhos públicos e estáveis:

- **Raw Input API**: `RegisterRawInputDevices` + `WM_INPUT` + `GetRawInputData` / `GetRawInputBuffer` (buffered, para mouse 1000 Hz). Entrega eventos HID crus de teclado/mouse/gamepad, independente do foco de janela clássico. Fonte: [Raw Input Overview](https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input), [GetRawInputData](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata). É o que jogos fullscreen usam — casa perfeitamente com o modelo 3b.
- Precisa de uma janela (mesmo invisível/fullscreen) para receber `WM_INPUT`, ou usar o modo sem-legacy do Raw Input. Isso é trivial mesmo no cenário fullscreen exclusive.
- **Mais baixo** (se um dia largar o loop de mensagens Win32): o pipeline de input do kernel passa por `win32k`/RIM (Raw Input Manager) e pelos drivers de classe HID (`hidclass.sys`, `kbdclass`, `mouclass`). Falar direto com `\Device\RawInputManager` ou com os drivers de classe é undocumented e desnecessário no início — Raw Input já resolve. Guardar para a fase "user space independente".

**Recomendação de input para o primeiro compositor:** Raw Input (buffered). Público, estável, latência baixa, bate com 3b.

---

## 6. Veredito honesto e caminho recomendado

**A afirmação "fazer o próprio DWM de cara" precisa ser desmembrada:**

- Se "próprio DWM" = **substituir o dwm.exe e compor as janelas Win32 existentes** → **(c) exige reescrever o stack gráfico**: não há API consumidora de redirection surfaces, o object database do DirectComposition no win32k espera um único motor por sessão, e nem o ReactOS conseguiu. É P&D de reversa de longo prazo, não "de cara". (Caminho 3a.)

- Se "próprio DWM" = **compositor próprio do NTUnix que mostra nossas janelas nativas na tela, com nosso window manager, ignorando o DWM** → **(a) realista já**, via **caminho 3b** (swap chain flip-model fullscreen/borderless em D3D11/DXGI, APIs públicas e estáveis) + **Raw Input** para entrada. Isso entrega literalmente "uma janela nativa própria na tela" com composição própria, sem tocar no DWM, sem driver de kernel. É o mesmo mecanismo de qualquer jogo — logo, robusto e documentado.

- **Caminho recomendado para o PRIMEIRO compositor: 3b.** Depois, quando quiser mais controle/independência do runtime DXGI e menor latência, evoluir para **3d(i)** (D3DKMT + `SetVidPnSourceOwner` exclusive). **3c está morto** no Win11 (sem XDDM, sem modo basic). **3d(ii) IddCx** é para "tela virtual/streaming", não para o desktop principal. **3a** fica como pesquisa futura (a "ponte para janelas Win32" que a própria VISAO.md §18 adia — e a pesquisa confirma que adiar é o certo).

Isso valida e refina o que `VISAO.md` §17–§18 já intuíam: **DWM como camada de compat para Win32, compositor próprio para o nativo** — a novidade concreta é *como* fazer o compositor próprio já: flip-model fullscreen, não substituição do DWM.

### Riscos e incertezas que SÓ um teste em VM resolve

1. **Independent flip vs. "sempre compõe":** confirmar empiricamente que uma borderless flip chain cobrindo 100% do monitor realmente bypassa o DWM na build alvo do Win11 (a heurística de independent flip é do driver e muda por versão/GPU). Medir com PresentMon.
2. **Fullscreen exclusive em Win11:** confirmar se `SetFullscreenState(TRUE)` ainda dá posse real da saída ou se a "Windowed Optimization" o converte em flip por baixo — e se isso importa para nós.
3. **Convivência com o DWM vivo:** ao assumir a saída, o que acontece com o dwm.exe (ele continua compondo por baixo? consome GPU à toa?) e como ceder/retomar a saída no alt-tab sem tela preta.
4. **VidPN ownership (3d):** testar `D3DKMTSetVidPnSourceOwner` exclusive de um processo user-mode comum — se o dxgkrnl deixa, e como recuperar se o processo morrer com a saída presa (risco de brickar a sessão).
5. **Multi-monitor e vblank sync** com nossa própria flip chain por monitor.
6. **Input em fullscreen exclusive:** confirmar Raw Input buffered funcionando com a nossa janela fullscreen e latência aceitável.
7. **Sem GPU/driver WDDM (Basic Display Adapter):** ver se o caminho flip-model degrada graciosamente no rasterizador de software.

> Nota metodológica: os pontos "não dá para desligar DWM", "composição obrigatória desde Win8", "redirection surface consumidora é privada", "XDDM removido/GDI-classic morto" e "flip model bypassa DWM" têm **fonte primária Microsoft** e são sólidos. Os pontos sobre *reiniciar dwm.exe* e *bypass por borderless* vêm de fóruns (fontes fracas) mas consistentes entre si — marcados como "confirmar em VM".

---

## 7. Fontes

Primárias (Microsoft Learn / Windows Driver Docs):
- DirectComposition — Architecture and components: https://learn.microsoft.com/en-us/windows/win32/directcomp/architecture-and-components
- DirectComposition — Bitmap objects (redirection/composition/swapchain): https://learn.microsoft.com/en-us/windows/win32/directcomp/bitmap-surfaces
- DwmDxGetWindowSharedSurface (redirection surface, Win7-only, driver-only): https://learn.microsoft.com/en-us/windows/win32/dwm/dwmdxgetwindowsharedsurface
- Enable and control DWM composition (seção "Disabling DWM composition (Windows 7 and earlier)"): https://learn.microsoft.com/en-us/windows/win32/dwm/composition-ovw
- Desktop Window Manager is always on (Compatibility Cookbook — mandatório desde Win8): https://learn.microsoft.com/en-us/windows/compatibility/desktop-window-manager-is-always-on
- DwmEnableComposition (deprecada): https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/nf-dwmapi-dwmenablecomposition
- For best performance, use DXGI flip model (Independent Flip bypassa DWM): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- DXGI flip model dev blog: https://devblogs.microsoft.com/directx/dxgi-flip-model/
- IDXGISwapChain::SetFullscreenState: https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-setfullscreenstate
- Desktop Duplication API (única leitura sancionada do desktop composto): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/desktop-dup-api
- WDDM in Windows 8 (XDDM removido): https://learn.microsoft.com/en-us/windows-hardware/drivers/display/wddm-in-windows-8
- D3DKMTSetVidPnSourceOwner (posse de saída de vídeo): https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/ns-d3dkmthk-_d3dkmt_setvidpnsourceowner
- D3DKMTPresent: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/d3dkmthk/nf-d3dkmthk-d3dkmtpresent
- Indirect Display Driver Model (IddCx): https://learn.microsoft.com/en-us/windows-hardware/drivers/display/indirect-display-driver-model-overview
- Raw Input Overview: https://learn.microsoft.com/en-us/windows/win32/inputdev/about-raw-input
- GetRawInputData: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getrawinputdata

Secundárias / referência técnica:
- ReactOS — An initial investigation into WDDM (NT sem DWM, CDD como XDDM): https://reactos.org/blogs/investigating-wddm/
- Wikipedia — Desktop Window Manager (GDI→CDD→redirection): https://en.wikipedia.org/wiki/Desktop_Window_Manager
- geoffchappell / win32k tree (ntuser + ntgdi): https://geob99.github.io/pages/doc/win32k-tree.html
- win32kbase.sys / win32kfull.sys split (Win10+): https://learn.microsoft.com/en-us/answers/questions/3723188/win32kbase-sys-and-win32kfull-sys-bsod

Fracas (fórum/comunidade — marcadas como "confirmar em VM"):
- WinClassic — disabling DWM: https://winclassic.net/thread/159/alternative-method-disabling-dwm-windows
- NoMoreDWM: https://github.com/TK50P/NoMoreDWM
- Blur Busters — borderless flip bypass: https://forums.blurbusters.com/viewtopic.php?t=12139&start=10
