# Desktop NTUnix (dispd + ntwm) — gaps conhecidos (deferidos)

Itens do review que foram **deliberadamente adiados** (com justificativa), depois
de corrigir todos os bloqueadores e a maioria dos bugs de correção. Nenhum destes
impede o uso do desktop; são features grandes, otimizações, ou hardening de baixo
risco no cenário atual (WinPE/live, single-monitor, VM sem GPU real).

## Renderização / display

- **Multi-monitor (#54).** `dispd` nasce em (0,0) com `SM_CXSCREEN/SM_CYSCREEN` e
  manda um único `OUTPUT`. Suportar vários monitores exige `EnumDisplayMonitors`,
  um `OUTPUT` por monitor e uma abstração `Monitor` no `ntwm` (cada um com seu
  layout/nmaster/mfact). A VM de teste é single-monitor → sem como validar agora.
- **Mudança de resolução/DPI (#53).** Sem `WM_DISPLAYCHANGE`/`WM_DPICHANGED`/
  `WM_SIZE`; backbuffer, janela raiz e swapchain ficam no tamanho do boot. Precisa
  tratar esses eventos + recriar backbuffer + chamar `present->resize` (hoje é
  código morto, parte do #58). Feature real, adiada.
- **Device loss do DXGI (#55).** `Present`/`Map`/`CopyResource` não checam
  `DXGI_ERROR_DEVICE_REMOVED`. Num GPU real, um TDR congelaria a imagem. Fix:
  detectar e recriar device/swapchain, ou cair no backend GDI. Só importa em
  hardware real com GPU (a VM roda WARP); adiado para quando houver bancada com GPU.
- **Damage tracking real (#57).** Hoje qualquer mudança recompõe a tela inteira e
  (no DXGI) sobe o frame inteiro pra GPU. O idle-present já limita a *frequência*
  (só recompõe quando sujo); falta limitar a *área* (regiões sujas). É otimização,
  não correção — adiada. Referência: `docs/pesquisa/deep-compositors.md` (damage ring).
- **>256 janelas (#52).** O compose usa `vis[256]` e o layout embutido `v[64]`.
  Irreal de atingir num desktop tiling; viraria arrays dinâmicos se algum dia
  importar. Baixa prioridade.
- **Código-placeholder (#58).** `Window.dirty`/`Window.visible` e
  `PresentBackend.resize` existem mas ainda não são exercitados (visibilidade é por
  `ws==cur_ws`, resize de tela é #53). Mantidos como ganchos para essas features;
  inofensivos.

## Terminal / VT

- **Render Unicode/UTF-8 (#63).** A ENTRADA já manda UTF-8 (WideCharToMultiByte),
  mas o renderer é single-byte (CP437 + `OEM_FIXED_FONT`), então multibyte aparece
  corrompido. Render Unicode de verdade precisa: grade guardando codepoints (não
  bytes), uma fonte TTF Unicode embutida (`AddFontMemResourceEx`) e text-out wide.
  É um bloco grande; adiado.
- **Truecolor (#62).** O parser reconhece `38;2;r;g;b` mas mapeia pra 16 cores.
  Suportar 24-bit exige `Cell` guardando RGB (não índice) + render por cor real.
  Adiado (TUIs modernas terão cores aproximadas até lá).

## Apps (fronteira apps↔dispd)

- **Double-buffering / vsync do app (#47).** `APP-COMMIT` não sincroniza: o app pode
  escrever na section enquanto o `dispd` faz `BitBlt` → tearing. Fix: protocolo com
  front/back surface (duas sections) e swap atômico no commit. Feature, adiada.
- **ACL da section (#48).** Os nomes `ntunix-appsurf-N` são previsíveis e a section
  é criada com DACL padrão — outro processo da mesma sessão poderia abrir e alterar
  os pixels de uma janela. Fix: `SECURITY_ATTRIBUTES` restritiva + nome aleatório.
  Hardening de segurança; num live single-user é baixo risco. Adiado.

## Input

- **Escopo do hook global (#20).** O `WH_KEYBOARD_LL` pega teclas do sistema todo;
  atalhos do WM poderiam ser roubados de outra janela Win32 em foreground. No alvo
  (WinPE/live, o `dispd` é dono da sessão) isso não acontece na prática. Um fix
  checaria se a janela em foreground pertence à nossa sessão antes de tratar. Baixa
  prioridade.

---

Tudo o mais do review foi corrigido (ver commits `feat/desktop-dispd-ntwm`). Os
bloqueadores nomeados no review — surface do app destruída no layout, input de
apps, foco/workspace divergentes, transações de layout, e hook bloqueante — estão
resolvidos.
