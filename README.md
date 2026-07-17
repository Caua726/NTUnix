# NTUnix

> Unix philosophy. NT foundation.

NTUnix é um sistema operacional experimental que utiliza o kernel NT, drivers e compatibilidade Windows como fundação, substituindo o user space tradicional por uma arquitetura modular inspirada em Linux e Unix.

**Em uma frase:** um user space Unix-like construído sobre o kernel NT, com compatibilidade Windows integrada.

## Como funciona a distribuição

NTUnix **não redistribui nada da Microsoft**. O usuário fornece uma **ISO oficial do Windows** e uma licença válida; a base de build deste repositório trata essa ISO localmente:

1. extrai a ISO de origem;
2. **remove o user space padrão do Windows** (apps provisionados, OneDrive, Edge integrado, wallpapers extras — ver `build/strip.list`);
3. **injeta a árvore do NTUnix** em `\NTUnix` dentro da imagem;
4. planta os hooks que trocam a experiência no primeiro boot (`autounattend.xml` + `SetupComplete.cmd`): o **shell da sessão vira o `ntsession`** no lugar do `explorer.exe`, e serviços/telemetria não essenciais são desligados;
5. reempacota uma ISO híbrida bootável (BIOS + UEFI).

O resultado boota, instala e cai direto no ambiente NTUnix — kernel NT e drivers da Microsoft embaixo, user space nosso em cima.

## Documentação

- [Visão geral e arquitetura](docs/VISAO.md) — o documento fundador.
- [Protocolo do initd](docs/PROTOCOLO.md) — pipe de controle, verbos e units.
- [Especificação da musl-nt](docs/musl-nt-spec.md) — ABI, syscalls e decisões.
- [Build e uso da musl-nt](musl-nt/README.md) — toolchain, testes e limitações.

## Status

**Fase 1 (ambiente hospedado) + base de build da ISO — primeiro corte funcional.**

| Componente | Estado |
|---|---|
| `initd` | ✅ supervisor: units, Job Objects, `Restart=` com throttle, deps `Requires=`, pipe de controle |
| `ntctl` | ✅ `list/status/start/stop/restart/enable/disable/logs/reload/ping/shutdown` |
| `logd` | ✅ mínimo: pipe `ntunix-logd` → `/var/log/system.log` |
| `demod` | ✅ serviço de demonstração (heartbeat + IPC com logd) |
| `ntsession` | ✅ shell da sessão: garante o initd, abre o terminal, se ressuscita |
| Base de build da ISO | ✅ `make-iso.sh` (strip + inject + repack), `autounattend.xml`, `SetupComplete.cmd` |
| Camada de caminhos | 🌱 semente: `/etc/...` → `<NTUNIX_ROOT>\etc\...` (`src/common/ntupath.c`) |
| `musl-nt` | ✅ libc LP64/PE sem UCRT; arquivos, memória, statfs e sockets Winsock validados sob Wine |
| BusyBox NT | ✅ mais de 80 applets compilados; shell standalone/no-fork e bateria de runtime |
| `ntpkg` | ⬜ ainda não começado |

## Build

Cross-compile de Linux com mingw-w64. A musl-nt também requer Clang/LLVM
(`llc`, `llvm-config` e headers). Para gerar a ISO: `wimlib`, `xorriso`, `7z`
(ou `bsdtar`). Wine é usado pelos testes.

```bash
# Arch: pacman -S mingw-w64-gcc wimlib xorriso p7zip
make               # compila os binários e monta a árvore staged em out/
make smoke         # roda o runtime sob Wine (19 checks)
make check-build   # valida a base de build sem precisar de ISO (lint + dry-run de imagem)
make musl-nt       # gera musl-nt/build/libc-nt.a + crt0.o
make musl-nt-test  # executa hello, smoke, allocator e rede; rejeita UCRT/MSVCRT
make busybox-nt    # compila e instala out/system/bin/busybox.exe
make busybox-nt-test # bateria ampla dos coreutils sob Wine
```

As fontes da musl e do BusyBox são externas. Os caminhos padrão são
`/tmp/musl-1.2.6` e `/tmp/bbsrc`; sobrescreva com `MUSL_SRC=` e
`BUSYBOX_SRC=`.

### Gerar a ISO do NTUnix

```bash
make iso WIN_ISO=/caminho/Win11_oficial.iso        # → NTUnix.iso
#   ou direto:  ./build/make-iso.sh Win11.iso saida.iso

./build/test-vm.sh NTUnix.iso                      # boota numa VM UEFI (QEMU/OVMF)
```

Variáveis úteis: `NTUNIX_EDITIONS="1 6"` limita quais edições da imagem tratar; `NTUNIX_WORK=/caminho` muda o diretório de trabalho.

## Rodando o runtime isolado (sem ISO)

Sob Wine ou num Windows real — a árvore `out/` é a raiz NTUnix:

```bash
wine out/system/bin/initd.exe &
wine out/system/bin/ntctl.exe list
wine out/system/bin/ntctl.exe shutdown
```

## Layout do repositório

```text
src/common/     ntu.h + ntupath/ntuini/ntuutil — compartilhado
src/initd/      supervisor (initd.c, service.c, pipesrv.c)
src/ntctl/      cliente de controle
src/logd/       coletor central de logs (v0)
src/demod/      serviço de demonstração
src/ntsession/  shell da sessão (substitui o explorer.exe)
etc/units/      units de exemplo (*.service)
etc/passwd      identidade Unix mínima (root, uid/gid 0)
etc/group       grupo Unix mínimo
etc/hosts       resolução local mínima para a musl
build/          make-iso.sh, autounattend.xml, SetupComplete.cmd, strip.list, test-vm.sh
musl-nt/        libc musl LP64 para PE + backend NT + toolchain e testes
test/           smoke.sh (runtime), build-check.sh (base de build)
docs/           VISAO.md, PROTOCOLO.md, musl-nt-spec.md
```
