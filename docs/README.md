# Documentação do NTUnix

## Contratos (normativos)

Estes descrevem o que o sistema **deve** fazer. Mudança de código que os
contrarie é bug — no código ou no documento.

| Documento | Assunto |
|---|---|
| [VISAO.md](VISAO.md) | Visão geral e arquitetura. O documento fundador — comece por aqui. |
| [PROTOCOLO.md](PROTOCOLO.md) | Protocolo de controle do `initd`: pipe, verbos, formato das units. |
| [musl-nt-spec.md](musl-nt-spec.md) | ABI da libc: LP64 sobre PE, syscalls do backend NT, decisões e limites. |

O protocolo `dispd`↔`ntwm` não tem documento próprio; o contrato vive em
[`src/common/ntuwm.h`](../src/common/ntuwm.h), e a fronteira apps↔dispd em
`src/dispd/appsrv.c`.

## Guias

| Documento | Assunto |
|---|---|
| [canal-debug-vm.md](canal-debug-vm.md) | Terminal de debug compartilhado da VM (`dbgterm`), via reverse shell TCP. Só dev. |
| [../musl-nt/README.md](../musl-nt/README.md) | Build, toolchain e testes da musl-nt. |

## Auditoria — [auditoria/](auditoria/)

Fotografia de um momento, não contrato. Envelhece conforme o código anda.

| Documento | Assunto |
|---|---|
| [auditoria/dispd-ntwm-prompt.md](auditoria/dispd-ntwm-prompt.md) | Manifesto da auditoria do desktop: escopo e método. |
| [auditoria/dispd-ntwm-relatorio.md](auditoria/dispd-ntwm-relatorio.md) | Relatório completo — os achados numerados que o resto referencia. |
| [auditoria/arquitetura-debt.md](auditoria/arquitetura-debt.md) | Os achados tipo `ARCH`: dívida deliberada, não bug. |
| [auditoria/desktop-known-gaps.md](auditoria/desktop-known-gaps.md) | O que o desktop garante e o que reconhecidamente ainda não faz. |

## Pesquisa — [pesquisa/](pesquisa/)

Notas de investigação que embasaram as decisões de projeto. Não são
especificação e não acompanham o código; são o "por quê" por trás dele.

- **NT por dentro** — [nt-native-api.md](pesquisa/nt-native-api.md),
  [nt-boot-install.md](pesquisa/nt-boot-install.md),
  [nt-win11-image.md](pesquisa/nt-win11-image.md),
  [nt-dwm-compositor.md](pesquisa/nt-dwm-compositor.md),
  [comparacao-linux-vs-nt.md](pesquisa/comparacao-linux-vs-nt.md)
- **Display server e WM** — [deep-wm-architecture.md](pesquisa/deep-wm-architecture.md),
  [deep-dwm.md](pesquisa/deep-dwm.md),
  [deep-xorg-server.md](pesquisa/deep-xorg-server.md),
  [deep-compositors.md](pesquisa/deep-compositors.md),
  [deep-gfx-server-linux.md](pesquisa/deep-gfx-server-linux.md),
  [deep-gfx-server-windows.md](pesquisa/deep-gfx-server-windows.md),
  [wm-compositor-survey.md](pesquisa/wm-compositor-survey.md)
- **BusyBox sobre a musl-nt** — [busybox-coreutils-musl-deps.md](pesquisa/busybox-coreutils-musl-deps.md)
  e os três levantamentos `bb-coreutils-*.txt` (funções de libc consumidas,
  headers públicos e o fecho completo de headers).
