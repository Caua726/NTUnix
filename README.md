# NTUnix

A Windows distribution with a Unix user space.

NTUnix keeps the Windows NT kernel and its drivers, and replaces everything
above them. The stock Windows user space is removed and a Unix-like one takes
its place: a service supervisor, a libc that talks to NT directly, a display
server, a tiling window manager, and BusyBox coreutils.

The relationship is the same one a Linux distribution has with the Linux
kernel. The kernel and the driver stack are not ours; the system built on top
of them is.

Windows programs keep working. They are discovered, stripped of their frame,
and tiled alongside everything else — here Task Manager sits in the master
column, with the clock and two BusyBox shells in the stack.

![Windows Task Manager tiled by ntwm, next to ntclock and two BusyBox shells](docs/screenshots/windows-app.png)

Version `0.1.0-dev`. It boots in a VM. It is not a system to depend on.

## What is in it

| | |
|---|---|
| `initd`     | Service supervisor. `.service` units, NT Job Objects with kill-on-close, `Restart=` with throttle, `Requires=` dependencies, `MemoryMax=`, control pipe. |
| `ntctl`     | Control client: `list status start stop restart enable disable logs reload ping shutdown`. |
| `ntsession` | Session shell, in place of `explorer.exe`. Starts initd, hands the screen to dispd, opens a recovery shell if the desktop does not appear. |
| `dispd`     | Display server and software compositor. Retained wallpaper/windows/layers/overlay scene, logical vs visual geometry, damage, adaptive blur/shadows and GDI/DXGI presentation. |
| `ntwm`      | Tiling policy process. Per-workspace dwindle/master state, geometric actions, rules, atomic reload and serialized frame protocol. Restarting it does not disturb surfaces. |
| `ntbar`     | Independent 34 px layer surface: workspace pills, focused title, layout and clock. |
| `ntwmctl`   | Request/response client for ntwm dispatchers and status. |
| `logd`      | Log collector. Appends to `/var/log/system.log` with a timestamp. |
| `musl-nt`   | The libc. musl 1.2.6 lowered to PE/x86-64 with no UCRT and no MSVCRT. Files, directories, stat, statfs, memory, processes, pty, signals, time, ioctl, sockets. `fork()` is absent by design. |
| BusyBox     | 86 applets, built against musl-nt without source patches. |

The libc is the part that makes the rest possible. musl is LP64 and Windows is
LLP64, so each translation unit is compiled as Linux/LP64, run through a
purpose-built LLVM tool that rewrites the calling convention, and lowered to
COFF. The NT backend underneath is about 4,700 lines.

The terminal runs on a native PTY over musl-nt, with libvterm as the VT engine.
ConPTY and console-scrape backends remain as fallbacks.

![A shell session under NTUnix](docs/screenshots/terminal.png)

Applications reach dispd through a shared section object, which is the NT
equivalent of `wl_shm`. `ntclock` is the only application using it so far — the
clock in the screenshots is drawing into a surface the compositor owns.

![ntclock in the master column against three shells](docs/screenshots/desktop.png)

## Requirements

Built on Linux, cross-compiled with mingw-w64. musl-nt also needs Clang and LLVM,
because it builds an LLVM tool of its own. Image work needs `wimlib`, `xorriso`
and `7z`, and the development VM needs `libvirt`, `qemu` and `samba`.

```sh
pacman -S mingw-w64-gcc clang llvm wimlib xorriso p7zip                       # Arch
apt install gcc-mingw-w64-x86-64 clang llvm wimlib-tools xorriso p7zip-full   # Debian
```

You also supply a Windows ISO. NTUnix redistributes no Microsoft code — the ISO
and the licence are yours, and the build treats them locally.

## Building

musl and BusyBox sources are not kept in the repository. `make deps` fetches
both into `build/deps/` against pinned sha256 sums; it is idempotent and runs
on its own before the targets that need it.

```sh
make                    # user space -> out/
make check-build        # lint and image dry-run, no ISO needed
make musl-nt            # libc-nt.a + crt0.o
make musl-nt-check      # builds the libc test battery; rejects UCRT/MSVCRT
make busybox-nt         # busybox.exe against musl-nt
make busybox-nt-check   # same check over busybox.exe
```

The `-check` targets are static: they run `objdump` over the output and fail if
anything imports UCRT, MSVCRT or `api-ms-win-crt`. Running the binaries is the
VM's job — NTUnix targets the NT kernel, so a real NT kernel is what it is
tested against.

## Installing

NTUnix installs itself. `ntstrap` is the installer, and it runs from inside the
live environment, the way `pacstrap` runs from the Arch ISO:

```sh
make iso                # live media + install.wim + ntstrap
```

Boot that media — it comes up in NTUnix — and from a terminal:

```sh
cmd /c ntstrap.cmd      # asks for the edition, confirms, then installs
```

`ntstrap` partitions the disk with `diskpart` (GPT: 300 MB ESP, 16 MB MSR, NTFS
for the rest), applies the image with `dism /Apply-Image`, strips the stock
Windows user space, copies the NTUnix tree in, loads the `SOFTWARE` and `SYSTEM`
hives **offline** to write the session shell and the rest of the configuration,
and calls `bcdboot`. All four tools are inbox in WinPE.

Windows Setup never runs. That is the point: there is no language page, no
product key prompt and no OOBE deciding anything, because none of that is ours
to answer. A machine that boots straight into `ntsession` because we wrote that
into the hive before its first boot is a different thing from a Windows
installation with the shell swapped afterwards.

The one exception is the local account, which cannot reasonably be created
offline; a minimal answer file covers the `oobeSystem` pass and nothing else.

`cmd /c` is needed because the NTUnix shell is BusyBox ash, which only executes
PE binaries — a `.cmd` needs the Windows interpreter.

## The development VM

Put a Windows ISO at `build/deps/windows.iso`, or pass `WIN_ISO=`.

```sh
make iso                # installer media
make vm-install         # boot it in the VM, so you can run ntstrap
make vm                 # afterwards: boot from disk, out/ mounted from the host
```

Once installed, the loop no longer involves an image at all: QEMU serves `out/` over
its built-in SMB server, the guest sees the host tree live, and a rebuild takes
effect on the next service restart.

```sh
make                              # on the host
ntctl restart dispd               # in the guest — picks up the new binary
```

SMB rather than virtiofs because Windows already has an SMB client: no driver to
install, so no signed-driver wall. That is the same reason the debug channel is
network rather than serial, which `docs/canal-debug-vm.md` records in detail.

An installed Windows also costs less RAM than running from the live media —
WinPE unpacks `boot.wim` into a ramdisk, so the image occupies memory for as
long as it runs.

```sh
make live               # live-only media, without install.wim (smaller)
make vm-live            # define the VM against it
```

Building the media extracts the source ISO, drops the Windows Setup image from
`boot.wim`, injects the NTUnix tree at `\NTUnix` with `ntstrap` alongside it,
points the WinPE shell at `ntsession`, and repacks a hybrid BIOS+UEFI ISO.

`OUT_ISO=` names the output, `VM_NAME=` and `VIRSH=` select a different VM,
`NTUNIX_VM_RAM=` and `NTUNIX_VM_CPUS=` size it. At runtime,
`NTUNIX_ROOT` sets the tree root, `DISPD_BACKEND=gdi|dxgi` picks the present
backend, and `DISPD_TERM=pty|conpty|scrape|demo` forces a terminal backend.

`make debug-live` adds a shared debug terminal reachable over TCP, documented in
`docs/canal-debug-vm.md`. It is unauthenticated and meant for development only.

## Current limits

`ls` returns nothing, with exit code 5, both bare and with a path, while shell
globbing over the same directory works. There is no package manager; `ntpkg` has
not been started, and there is no package format or install path.

The DXGI present backend runs but has not demonstrated its purpose: in a VM
without WDDM it falls back to software and never engages Independent Flip, which
is the reason the backend exists. GDI is the default and the guaranteed path.

Native Win32 windows are tiled in the manner of komorebi — discovered, unframed,
snapped — but the filter is heuristic, and UWP, DPI and minimise are unhandled.
Path translation from `/etc/x` to `<NTUNIX_ROOT>\etc\x` is a seed rather than a
VFS. `logd` handles one client at a time and does not rotate.

## Layout

```
src/common/     ntu.h · ntuwm.h · ntuapp.h · ntupath · ntuini · ntuutil
src/initd/      initd · service · pipesrv        src/ntctl/   control client
src/logd/       log collector                    src/demod/   demo service
src/ntsession/  session shell (replaces explorer.exe)
src/dispd/      compositor · present (gdi/dxgi) · vt · term (pty/conpty/scrape)
                · input · wmproto (dispd<->wm) · appsrv (apps<->dispd) · foreign
src/ntwm/       policy: state/layout · config/rules · actions · IPC · protocol
src/apps/       ntclock · ntbar · ntwmctl
musl-nt/        nt/ (the NT backend) · tools/ (LP64->COFF rewrite) · test/
third_party/    libvterm (MIT) — the VT engine from vim/neovim
etc/            units/*.service · passwd · group · hosts · mtab · ntwm.conf
build/          fetch-deps · make-live · ntstrap.cmd (the installer) · strip.list
test/           build-check.sh · desktop-check.sh (desktop fixtures under Wine)
docs/           contracts, audit, research — see docs/README.md
```

The normative documents are `docs/VISAO.md` for the founding architecture,
`docs/PROTOCOLO.md` for initd, `docs/musl-nt-spec.md` for libc and
`docs/DESKTOP.md` for the desktop state and wire contracts.

## License

MIT — © Cauã Alvarenga Neves.

Third-party code keeps its own terms: libvterm (MIT) is vendored under
`third_party/`; musl (MIT) and BusyBox (GPL-2.0) are fetched at build time and
never committed. An image produced by `make iso` or `make live` contains
BusyBox, so redistributing that image carries the GPL-2.0 obligations.
