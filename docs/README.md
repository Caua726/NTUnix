# Documentation

## Contracts

Normative. Code that contradicts these is a bug — in the code or in the
document.

- [`VISAO.md`](VISAO.md) — architecture and the founding design. Start here.
- [`PROTOCOLO.md`](PROTOCOLO.md) — initd control protocol: pipe, verbs, unit format.
- [`musl-nt-spec.md`](musl-nt-spec.md) — libc ABI: LP64 over PE, NT syscall backend, limits.
- [`DESKTOP.md`](DESKTOP.md) — ntwm/dispd/ntbar state, layouts, protocols, config and shortcuts.

The desktop wire constants live in
[`src/common/ntuwm.h`](../src/common/ntuwm.h) and
[`src/common/ntuapp.h`](../src/common/ntuapp.h).

## Guides

- [`canal-debug-vm.md`](canal-debug-vm.md) — the shared VM debug terminal (`dbgterm`), over a TCP reverse shell. Dev only.
- [`../musl-nt/README.md`](../musl-nt/README.md) — libc build, toolchain, tests.

## Audit — [`auditoria/`](auditoria/)

A snapshot, not a contract. Ages as the code moves.

- [`dispd-ntwm-relatorio.md`](auditoria/dispd-ntwm-relatorio.md) — the full report; the numbered findings everything else cites.
- [`dispd-ntwm-prompt.md`](auditoria/dispd-ntwm-prompt.md) — scope and method of that audit.
- [`arquitetura-debt.md`](auditoria/arquitetura-debt.md) — the `ARCH` findings: deliberate debt, not bugs.
- [`desktop-known-gaps.md`](auditoria/desktop-known-gaps.md) — what the desktop guarantees and what it admits it does not do.

## Research — [`pesquisa/`](pesquisa/)

Investigation notes behind the design decisions. Not specification, and they do
not track the code — they are the reasoning that preceded it.

- **NT internals** — `nt-native-api.md`, `nt-boot-install.md`, `nt-win11-image.md`, `nt-dwm-compositor.md`, `comparacao-linux-vs-nt.md`
- **Display server and WM** — `deep-wm-architecture.md`, `deep-dwm.md`, `deep-xorg-server.md`, `deep-compositors.md`, `deep-gfx-server-linux.md`, `deep-gfx-server-windows.md`, `wm-compositor-survey.md`
- **BusyBox over musl-nt** — `busybox-coreutils-musl-deps.md` plus the three `bb-coreutils-*.txt` surveys (libc functions consumed, public headers, full header closure)
