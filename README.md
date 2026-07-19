# NTUnix

> Unix philosophy. NT foundation.

NTUnix é um sistema operacional experimental que mantém o **kernel NT e os
drivers do Windows** como fundação, e substitui todo o user space por uma
arquitetura modular inspirada em Linux e Unix: um supervisor de serviços, uma
libc própria, um display server, um window manager em tiling e coreutils
BusyBox.

**Em uma frase:** um user space Unix-like construído sobre o kernel NT, com
compatibilidade Windows integrada.

Estado: **`0.1.0-dev`** — experimental, boota em VM, não use como sistema
principal.

## Por que

O kernel NT é bom. O que afasta quem vem de Unix é o user space em volta dele:
o modelo de processos, o shell, a ausência de composabilidade, a telemetria. O
NTUnix parte da premissa de que dá pra manter o kernel e os drivers — que
resolvem hardware e compatibilidade melhor do que qualquer projeto de hobby
conseguiria — e trocar apenas a camada de cima.

Isso significa que nada aqui roda sobre uma camada de emulação. A `musl-nt` é
uma libc que fala com o NT direto; o `dispd` desenha na tela pelo GDI/DXGI; o
`initd` supervisiona com Job Objects nativos.

## Como funciona a distribuição

NTUnix **não redistribui nada da Microsoft**. Você fornece uma **ISO oficial do
Windows** e uma licença válida; a base de build trata essa ISO localmente:

1. extrai a ISO de origem;
2. **remove o user space padrão do Windows** (apps provisionados, OneDrive, Edge
   integrado, wallpapers extras — ver [`build/strip.list`](build/strip.list));
3. **injeta a árvore do NTUnix** em `\NTUnix` dentro da imagem;
4. planta os hooks de primeiro boot ([`autounattend.xml`](build/autounattend.xml)
   + [`SetupComplete.cmd`](build/SetupComplete.cmd)): o **shell da sessão vira o
   `ntsession`** no lugar do `explorer.exe`, e serviços/telemetria não essenciais
   são desligados;
5. reempacota uma ISO híbrida bootável (BIOS + UEFI).

O resultado boota, instala e cai direto no ambiente NTUnix — kernel NT e drivers
da Microsoft embaixo, user space nosso em cima.

## Status

Coluna "estado" é o que o código faz hoje, não o que a arquitetura pretende.

### Base do sistema

| Componente | Estado | O que é |
|---|---|---|
| `initd` | ✅ funcional | Supervisor: units `.service`, Job Objects com kill-on-close, `Restart=` com throttle, deps `Requires=`, `MemoryMax=`, pipe de controle. O componente mais maduro. |
| `ntctl` | ✅ funcional | Cliente do pipe: `list/status/start/stop/restart/enable/disable/logs/reload/ping/shutdown`. |
| `ntsession` | ✅ funcional | Shell da sessão (substitui o `explorer.exe`): garante o initd, cede a tela ao dispd, e abre um shell de recuperação se o desktop não subir em ~15s. |
| `logd` | 🌱 v0 | Pipe → `/var/log/system.log` com timestamp. Um cliente por vez, sem rotação, sem prioridade. |
| `demod` | ✅ funcional | Serviço de demonstração (heartbeat + IPC com o logd). Existe para o smoke test exercitar supervisão. |
| `ntpkg` | ⬜ não começado | Gerenciador de pacotes. |

### Desktop

| Componente | Estado | O que é |
|---|---|---|
| `dispd` | ✅ funcional | Display server e compositor (~3.700 linhas). Uma janela raiz real; cada janela do desktop é uma superfície lógica com DIB próprio. Damage tracking, blur/glass, cantos arredondados, animações, barra de status, abas. |
| `ntwm` | ✅ funcional | Window manager em tiling derivado do dwm: master/stack, `mfact`, 9 workspaces, floating. Cliente externo e descartável — `ntctl restart ntwm` não derruba o dispd. |
| Terminal | ✅ funcional | PTY nativo (o caminho principal) sobre a musl-nt, com libvterm como engine VT. Backends alternativos: ConPTY, scrape e demo. |
| Fronteira apps↔dispd | 🌱 semente | Section object compartilhado (zero-copy, análogo NT do `wl_shm`). Funciona, mas o `ntclock` é o único cliente que existe. |
| Janelas nativas Win32 | 🚧 parcial | Modelo komorebi/GlazeWM: descobre, remove moldura e tila janelas do Windows. Filtro de janela é heurístico; sem tratamento de UWP, DPI ou minimizar. |
| Backend DXGI | 🚧 não validado | Flip-model carregado por `GetProcAddress`. Funciona, mas em VM sem WDDM cai em software e **não engata Independent Flip** — o bypass do DWM, que é a razão de existir do backend, segue não comprovado. GDI é o default e o fallback garantido. |

### Runtime

| Componente | Estado | O que é |
|---|---|---|
| `musl-nt` | ✅ funcional | Port da musl 1.2.6 para PE x86-64 **sem UCRT/MSVCRT**. Resolve o conflito LP64 (musl) vs LLP64 (Windows) compilando cada TU como Linux/LP64 e remarcando a convenção de chamada com uma ferramenta LLVM própria. `fork()` é deliberadamente indisponível. |
| BusyBox NT | ✅ funcional | 80+ applets compilados contra a musl-nt **sem patch de código**, shell standalone/no-fork, com bateria de runtime sob Wine. |
| Camada de caminhos | 🌱 semente | `/etc/x` → `<NTUNIX_ROOT>\etc\x`. Tradução mínima em [`src/common/ntupath.c`](src/common/ntupath.c). |

## Build

Cross-compile de Linux com mingw-w64. A `musl-nt` também requer **Clang/LLVM**
(`llc`, `llvm-config` e headers) — ela usa uma ferramenta LLVM própria para
reescrever a convenção de chamada. Para gerar a ISO: `wimlib`, `xorriso`, `7z`
(ou `bsdtar`). Wine é usado pelos testes.

```bash
# Arch:   pacman -S mingw-w64-gcc clang llvm wimlib xorriso p7zip
# Debian: apt install gcc-mingw-w64-x86-64 clang llvm wimlib-tools xorriso p7zip-full
```

As fontes da musl e do BusyBox são externas e **não ficam no repositório**.
`make deps` baixa as duas para `build/deps/` com verificação de sha256; é
idempotente e roda automaticamente antes dos alvos que precisam delas.

```bash
make deps            # baixa musl + busybox para build/deps/ (automático)
make                 # compila os binários e monta a árvore staged em out/
make smoke           # roda o runtime sob Wine (19 checks)
make check-build     # valida a base de build sem precisar de ISO (lint + dry-run)

make musl-nt         # gera musl-nt/build/libc-nt.a + crt0.o
make musl-nt-test    # hello, smoke, allocator e rede; rejeita UCRT/MSVCRT
make busybox-nt      # compila e instala out/system/bin/busybox.exe
make busybox-nt-test # bateria ampla dos coreutils sob Wine

make clean           # remove out/ e build/work (não mexe em build/deps)
```

Para sobrescrever a origem das fontes: `MUSL_SRC=/caminho` e `BUSYBOX_SRC=/caminho`.

### Gerar a imagem

Ponha a ISO oficial do Windows em `build/deps/windows.iso` — ou passe
`WIN_ISO=/caminho.iso` em qualquer um dos alvos abaixo. Os dois fazem o build
completo (deps + libc + busybox + binários) antes de empacotar.

```bash
make live            # ISO LIVE (WinPE); reinicia a VM libvirt ao final
make live NO_BOOT=1  # o mesmo, sem tocar na VM
make iso             # ISO instalável (aplica o Windows completo, estilo pacstrap)

./build/test-vm.sh NTUnix.iso   # boota numa VM UEFI (QEMU/OVMF)
```

Variáveis úteis: `OUT_ISO=` muda o arquivo de saída, `NTUNIX_EDITIONS="1 6"`
limita quais edições da imagem tratar, `NTUNIX_WORK=/caminho` muda o diretório
de trabalho, `VM_NAME=` e `VIRSH=` apontam para outra VM.

Para o canal de debug da VM (`make debug-live`), ver
[docs/canal-debug-vm.md](docs/canal-debug-vm.md). É só dev, sem autenticação —
nunca em build de produção.

## Rodando o runtime isolado (sem ISO)

Sob Wine ou num Windows real — a árvore `out/` é a raiz NTUnix:

```bash
wine out/system/bin/initd.exe &
wine out/system/bin/ntctl.exe list
wine out/system/bin/ntctl.exe shutdown
```

O desktop (`dispd` + `ntwm`) precisa de uma sessão gráfica real; sob Wine ele
sobe, mas o caminho testado é a VM.

### Variáveis de runtime

| Variável | Valores | Efeito |
|---|---|---|
| `NTUNIX_ROOT` | caminho | Raiz da árvore. Se ausente, é deduzida subindo a partir de `\system\bin`. |
| `DISPD_BACKEND` | `gdi` \| `dxgi` | Backend de apresentação. Default `gdi`. |
| `DISPD_TERM` | `pty` \| `conpty` \| `scrape` \| `demo` | Força o backend de terminal. Default: heurística por cmdline. |

## Layout do repositório

```text
src/common/         ntu.h, ntuwm.h + ntupath/ntuini/ntuutil — compartilhado
src/initd/          supervisor (initd.c, service.c, pipesrv.c)
src/ntctl/          cliente de controle
src/logd/           coletor central de logs (v0)
src/demod/          serviço de demonstração
src/ntsession/      shell da sessão (substitui o explorer.exe)
src/dispd/          display server + compositor, terminal, protocolo do WM
src/ntwm/           window manager em tiling (cliente do dispd)
src/apps/ntclock/   app de demo da fronteira apps↔dispd
src/ntdbgcon/       reverse shell de debug — aposentado pelo dbgterm do dispd
musl-nt/            libc musl LP64 para PE + backend NT + toolchain e testes
third_party/        libvterm (MIT) — engine VT do vim/neovim
etc/units/          units de exemplo (*.service)
etc/                passwd, group, hosts, mtab, ntwm/ntwm.conf
proc/mounts         stub de compatibilidade para o BusyBox (df, mount)
build/              fetch-deps.sh, make-iso.sh, make-live.sh, autounattend.xml,
                    SetupComplete.cmd, strip.list, test-vm.sh, vm-setup.sh
test/               smoke.sh (runtime), build-check.sh (base de build)
docs/               contratos, guias, auditoria e pesquisa — ver docs/README.md
```

## Documentação

Índice completo em **[docs/README.md](docs/README.md)**. Os três contratos:

- [Visão geral e arquitetura](docs/VISAO.md) — o documento fundador.
- [Protocolo do initd](docs/PROTOCOLO.md) — pipe de controle, verbos e units.
- [Especificação da musl-nt](docs/musl-nt-spec.md) — ABI, syscalls e decisões.

Build e uso da libc: [musl-nt/README.md](musl-nt/README.md).

## Licença

Código do NTUnix sob **[MIT](LICENSE)**.

Software de terceiros mantém a licença de origem:

- **libvterm** (MIT) — vendorizada em [`third_party/libvterm`](third_party/libvterm),
  ver [`third_party/libvterm/LICENSE`](third_party/libvterm/LICENSE).
- **musl** (MIT) e **BusyBox** (GPL-2.0) — não ficam no repositório; `make deps`
  baixa as fontes originais. Uma imagem gerada por `make iso`/`make live`
  **contém o BusyBox**, então redistribuir essa imagem carrega as obrigações da
  GPL-2.0 (entre elas, oferecer o código-fonte correspondente).
- **Windows** — nada da Microsoft é redistribuído. A ISO de origem e a licença
  são suas.
