# NTUnix

> Unix philosophy. NT foundation.

An experimental operating system that keeps the Windows NT kernel and its
drivers, and replaces everything above them. The Windows user space is stripped
out and a Unix-like one is built in its place: a service supervisor, a libc that
talks to NT directly, a display server, a tiling window manager, and BusyBox
coreutils.

Nothing here runs on an emulation layer. `musl-nt` is a real libc port — musl
1.2.6 lowered to PE/x86-64 with no UCRT and no MSVCRT — and BusyBox builds
against it unpatched, 80+ applets. `dispd` draws through GDI or DXGI. `initd`
supervises with NT Job Objects.

Around 8,000 lines of C across the user space, plus 4,700 in the musl-nt NT
backend. x86_64, cross-compiled from Linux.

```sh
make deps && make        # user space -> out/
make smoke               # 19 runtime checks under Wine
make live                # bootable live ISO from your own Windows ISO
```

## How the image is built

No Microsoft code is redistributed. You supply an official Windows ISO and a
valid licence; the build treats it locally — strip the stock user space (`build/strip.list`),
inject the NTUnix tree at `\NTUnix`, plant the first-boot hooks that make
`ntsession` the session shell instead of `explorer.exe`, and repack a hybrid
BIOS+UEFI ISO. It boots, installs, and lands directly in NTUnix.

## Components

| | Lines | |
|---|---|---|
| `initd`     | 861  | Supervisor: `.service` units, Job Objects with kill-on-close, `Restart=` with throttle, `Requires=` deps, `MemoryMax=`, control pipe. The most mature piece. |
| `dispd`     | 5610 | Display server and compositor. One real root window; every desktop window is a logical surface with its own DIB. Damage tracking, blur, rounded corners, open/close animation, status bar, tabs. |
| `ntwm`      | 662  | Tiling window manager derived from dwm — master/stack, `mfact`, 9 workspaces, floating. An external client, and disposable: `ntctl restart ntwm` does not take the desktop down. |
| `ntctl`     | 93   | Control client: `list status start stop restart enable disable logs reload ping shutdown`. |
| `ntsession` | 117  | Session shell replacing `explorer.exe`. Brings up initd, hands the screen to dispd, opens a recovery shell if the desktop does not come up in ~15s. |
| `logd`      | 76   | Pipe to `/var/log/system.log`, timestamped. One client at a time, no rotation, no priorities. |
| `musl-nt`   | 4731 | The libc. Resolves LP64 (musl) against LLP64 (Windows) by compiling each TU as Linux/LP64 and rewriting the calling convention with a purpose-built LLVM tool, then lowering to COFF. File, dir, stat, statfs, mem, proc, pty, signal, time, ioctl and sockets. `fork()` is deliberately absent. |

The terminal runs on a native PTY over musl-nt with libvterm as the VT engine;
ConPTY and console-scrape backends exist as fallbacks (`DISPD_TERM=pty|conpty|scrape|demo`).
Apps reach dispd through a shared section object — zero-copy, the NT analogue of
`wl_shm` — with `ntclock` as the one client so far.

## What's not there yet

`ntpkg`, the package manager, has not been started; there is no package format
and no way to install anything.

The DXGI present backend works but has not proven its point: in a VM without
WDDM it falls back to software and never engages Independent Flip, which is the
whole reason the backend exists. GDI is the default and the guaranteed path.

Native Win32 windows are tiled komorebi-style — discovered, unframed, snapped —
but the filter is heuristic and there is no handling for UWP, DPI, or minimise.
Path translation (`/etc/x` to `<NTUNIX_ROOT>\etc\x`) is a seed, not a VFS.
`logd` is a timestamped append and nothing more.

## Build and test

Cross-compiled from Linux with mingw-w64. musl-nt additionally needs Clang and
LLVM — it builds an LLVM tool to rewrite calling conventions. ISO work needs
`wimlib`, `xorriso` and `7z`. Wine runs the tests.

```sh
pacman -S mingw-w64-gcc clang llvm wimlib xorriso p7zip          # Arch
apt install gcc-mingw-w64-x86-64 clang llvm wimlib-tools xorriso p7zip-full   # Debian
```

musl and BusyBox sources stay out of the repo. `make deps` fetches both into
`build/deps/` with pinned sha256, is idempotent, and runs automatically before
the targets that need it.

```sh
make                    # user space -> out/
make smoke              # 19 runtime checks under Wine
make check-build        # lint + image dry-run, no ISO required
make musl-nt            # libc-nt.a + crt0.o
make musl-nt-test       # hello, smoke, allocator, network; rejects UCRT/MSVCRT
make busybox-nt         # busybox.exe against musl-nt
make busybox-nt-test    # coreutils battery under Wine
```

Put an official Windows ISO at `build/deps/windows.iso`, or pass `WIN_ISO=`.
Both image targets do the full build first.

```sh
make live               # live ISO (WinPE); restarts the libvirt VM
make live NO_BOOT=1     # same, without touching the VM
make iso                # installable ISO, applies full Windows, pacstrap-style
./build/test-vm.sh NTUnix.iso    # boot in a UEFI VM (QEMU/OVMF)
```

`OUT_ISO=` sets the output file, `NTUNIX_EDITIONS="1 6"` limits which image
editions are processed, `VM_NAME=` and `VIRSH=` point at a different VM. The VM
debug channel (`make debug-live`) is documented in `docs/canal-debug-vm.md` — dev
only, unauthenticated.

To run the user space without an ISO, under Wine or real Windows, `out/` is the
NTUnix root:

```sh
wine out/system/bin/initd.exe &
wine out/system/bin/ntctl.exe list
```

Runtime knobs: `NTUNIX_ROOT` (tree root, otherwise derived from `\system\bin`),
`DISPD_BACKEND=gdi|dxgi`, `DISPD_TERM=pty|conpty|scrape|demo`.

## Layout

```
src/common/     ntu.h · ntuwm.h · ntupath (path translation) · ntuini · ntuutil
src/initd/      initd · service · pipesrv        src/ntctl/   control client
src/logd/       log collector                    src/demod/   demo service
src/ntsession/  session shell (replaces explorer.exe)
src/dispd/      compositor · present (gdi/dxgi) · vt · term (pty/conpty/scrape)
                · input · wmproto (dispd<->wm) · appsrv (apps<->dispd) · foreign
src/ntwm/       tiling wm: ntwm · proto · layout
src/apps/       ntclock (demo client of the app surface API)
musl-nt/        nt/ (the NT backend) · tools/ (LP64->COFF rewrite) · test/
third_party/    libvterm (MIT) — the VT engine from vim/neovim
etc/            units/*.service · passwd · group · hosts · mtab · ntwm.conf
build/          fetch-deps · make-iso · make-live · autounattend.xml · strip.list
test/           smoke.sh (runtime) · build-check.sh (image base)
docs/           contracts, audit, research — see docs/README.md
```

Three documents are normative: `docs/VISAO.md` (architecture),
`docs/PROTOCOLO.md` (the initd control protocol), `docs/musl-nt-spec.md` (libc
ABI and syscalls). Code that contradicts them is a bug, in one or the other.

## License

MIT — © Cauã Alvarenga Neves.

Third-party code keeps its own: libvterm (MIT) is vendored under `third_party/`;
musl (MIT) and BusyBox (GPL-2.0) are fetched at build time, never committed. An
image produced by `make iso` or `make live` contains BusyBox, so redistributing
that image carries the GPL-2.0 obligations.
