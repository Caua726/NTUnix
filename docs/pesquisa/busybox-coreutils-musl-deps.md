# BusyBox coreutils sobre musl-nt — o que implementar desde já

Análise dos fontes reais: **BusyBox 1.36.1** (`/usr/bin/busybox` + clone
`git@github.com/mirror/busybox`) e **musl** (a que vem no Zig 0.16, em
`.../lib/libc/musl`). Objetivo: o conjunto MÍNIMO de syscalls Linux + funções
libc que o `musl-nt` precisa emular sobre o NT (Win32 primeiro, ntdll nos
gaps) para o `cat` e depois o `ls` do BusyBox rodarem.

> Escopo: applets de **coreutils** apenas. Networking/init/shell ignorados.

---

## 0. O ponto único de interceptação (a chave de tudo)

A musl fala com o kernel por **um só lugar**: `__syscall(n, ...)`, definido em
`arch/x86_64/syscall_arch.h` como `syscall` inline (asm). Todo `open`, `read`,
`stat`… no fim vira `__syscall(SYS_xxx, ...)`.

**O `musl-nt` = reescrever esse `__syscall` como um `switch (n)` que despacha
cada número para o nosso backend Win32/ntdll.** Não tocamos no resto da musl
(stdio, malloc, string, printf ficam intactos — herdamos de graça).

Além do `__syscall`, três peças de arranque precisam adaptação:
- `arch/x86_64/crt_arch.h` — o `_start` (entry point). No PE o entry e a ABI de
  argumentos são outros: escrever um `_start` PE que monta `argc/argv/envp` e
  chama `__libc_start_main`.
- `src/env/__libc_start_main.c` — roda os inicializadores e chama `main`. Quase
  reusável; depende de `__init_tls` e `__syscall(SYS_set_tid_address)`.
- `src/env/__init_tls.c` + TLS — a musl monta o TLS via `arch_prctl`. No NT o
  thread já tem TEB/TLS; **adaptar o TLS ao modelo NT é o ponto mais delicado
  do arranque** (não é uma syscall, é layout de thread).

Cortar a UCRT: hoje o mingw linka `api-ms-win-crt-*`. O `musl-nt` linka
`-nostdlib` + a nossa `libc.a` (musl) + o nosso `crt0` PE. É a construção do
target `x86_64-w64-ntposix`.

---

## 1. `cat` — o mínimo absoluto (verificado em `coreutils/cat.c`)

O núcleo do `cat` é trivial:

```
fd = open(arquivo)              // ou stdin (fd 0)
while ((n = read(fd, buf, 4K))) // COMMON_BUFSIZE
    write(1, buf, n)            // full_write → write
close(fd)
```

As opções `-n/-e/-v/-t` só adicionam **processamento em memória** do buffer —
zero syscalls novas.

**Funções libc:** `open`, `read`, `write`, `close`. Mais o arranque e o
`write(2, ...)` pra mensagens de erro.

**Syscalls que a musl emite:**

| syscall | usado para |
|---|---|
| `openat(AT_FDCWD, path, flags)` | `open()` (musl sempre usa openat) |
| `read(fd, buf, n)` | ler |
| `write(fd, buf, n)` | escrever |
| `close(fd)` | fechar |
| `mmap(anon)` | `malloc` da musl (arena inicial) |
| `ioctl(fd, TIOCGWINSZ)` | musl decide line-buffering do stdout/stderr |
| `exit_group(code)` | terminar |

> Nota: o `cat` escreve com `write` direto (não `printf`), mas a **detecção de
> tty** do stdio da musl (`__stdout_write`) faz um `ioctl(TIOCGWINSZ)`. Basta um
> stub que devolve o tamanho do console (ou `ENOTTY`).

---

## 2. `ls` — o próximo degrau (o que ele adiciona sobre o `cat`)

Fluxo do `ls`: listar diretório + `stat` de cada entrada + formatar em colunas.

**Syscalls NOVAS vs. `cat`:**

| syscall | musl (verificado) | usado para |
|---|---|---|
| `getdents(fd, buf, n)` | `src/dirent/readdir.c:15` | `opendir/readdir` — listar o dir |
| `statx(fd, path, flag, mask, &stx)` | `src/stat/fstatat.c:40` | `lstat`/`stat` — tipo, tamanho, tempos, modo |
| `readlinkat(fd, path, buf, n)` | `readlink()` | alvo de symlinks (`-l`) |
| `ioctl(1, TIOCGWINSZ)` | | largura do terminal p/ colunas |

E, só para `ls -l` (dono/grupo), a musl resolve nomes via NSS, que **abre e lê
`/etc/passwd` e `/etc/group`** (`open`/`read`/`close` de novo) em `getpwuid`/
`getgrgid`. Sem esses arquivos, cai pra número do uid/gid.

> **A musl usa `statx`, não o `stat` legado** — o nosso emulador tem que
> preencher a `struct statx` (não a `stat` antiga). Confirmado no fonte.

---

## 3. Mapeamento syscall → NT (Win32 primeiro, ntdll no gap)

### (a) Mapeiam LIMPO para Win32
| syscall Linux | Win32 |
|---|---|
| `openat` | `CreateFileW` (traduzir flags→access/creation; `AT_FDCWD`+relativo pelo cwd) |
| `read` | `ReadFile` |
| `write` | `WriteFile` |
| `close` | `CloseHandle` |
| `lseek` | `SetFilePointerEx` |
| `mmap(anon)` / `munmap` | `VirtualAlloc` / `VirtualFree` |
| `mmap(arquivo)` | `CreateFileMapping` + `MapViewOfFile` |
| `ioctl(TIOCGWINSZ)` / `isatty` | `GetConsoleScreenBufferInfo` / `GetFileType`==`FILE_TYPE_CHAR` |
| `clock_gettime` / `gettimeofday` | `QueryPerformanceCounter` / `GetSystemTimePreciseAsFileTime` |
| `exit_group` | `ExitProcess` |
| `getpid` | `GetCurrentProcessId` |
| `rename`,`unlink`,`mkdir`,`rmdir` | `MoveFileEx`,`DeleteFile`,`CreateDirectory`,`RemoveDirectory` (p/ rm/mv/mkdir) |

### (b) Melhor via ntdll
| syscall | ntdll | por quê |
|---|---|---|
| `getdents64` | `NtQueryDirectoryFile` | devolve várias entradas num buffer (casa com getdents); converter `FILE_*DIRECTORY_INFORMATION`→`dirent64` (d_ino/d_type/d_name). Win32 `FindFirstFile` funciona mas é uma-por-vez. |
| `statx` | `NtQueryInformationFile`/`GetFileInformationByHandleEx` | tamanho/tempos vêm limpos; **`st_mode`/`uid`/`gid` são FABRICADOS** (ver c) |
| `readlinkat` | `DeviceIoControl(FSCTL_GET_REPARSE_POINT)` | symlinks NTFS são reparse points |

### (c) PROBLEMÁTICOS / sem equivalente (fabricar ou adiar)
- **Permissões Unix (`st_mode`/`uid`/`gid`)**: o NT usa ACL/SID, modelo
  incompatível. Fabricar: dir=`040755`, arquivo=`0100644`, uid=gid=0. Suficiente
  pro `ls` mostrar algo coerente; permissão real fica pós-v0.
- **`fork`**: hostil ao NT (já decidido não fazer). `cat`/`ls` **não usam fork**
  — a maioria do coreutils é single-process. `posix_spawn`→`CreateProcess`
  cobre os que precisam (não é o caso aqui).
- **`/proc`, ioctls de terminal raw (`TCGETS/TCSETS`)**: `stty`/`less` usam;
  `cat`/`ls` não. Fora do caminho crítico.
- **TLS via `arch_prctl`**: não é syscall de dados, é arranque — resolver no
  `__init_tls` adaptado ao TEB do NT (item 0).

---

## 4. Ordem de ataque (o acionável)

**Nível 0 — arranque + `cat` imprime um arquivo:**
1. `__syscall` dispatcher: o `switch (n)` — o coração do `musl-nt`.
2. `crt0` PE (`_start`) + `__libc_start_main` + `__init_tls` no modelo NT; linkar
   `-nostdlib` cortando a UCRT.
3. Syscalls do `cat`: `openat`→CreateFile, `read`→ReadFile, `write`→WriteFile,
   `close`→CloseHandle, `mmap(anon)`→VirtualAlloc (malloc da musl),
   `exit_group`→ExitProcess, stub de `ioctl(TIOCGWINSZ)`, `writev`.
   → **`busybox cat arquivo` roda.**

**Nível 1 — `ls` lista um diretório:**
4. `getdents64` via `NtQueryDirectoryFile` → `dirent64`.
5. `statx` via `GetFileInformationByHandleEx` → `struct statx` (com `st_mode`
   fabricado).
6. `ioctl(TIOCGWINSZ)` real (`GetConsoleScreenBufferInfo`) + `isatty`.
7. `readlinkat` (reparse points) — pode ser stub no começo.
   → **`busybox ls` (sem `-l`) roda.** `ls -l` precisa de `/etc/passwd`/`/etc/group`
   (criar arquivos fake) ou cai pra uid/gid numérico.

**Depois disso**, cada applet novo (`cp`,`mv`,`rm`,`wc`,`head`,`tail`,`echo`,
`pwd`,`mkdir`…) reusa quase tudo; os incrementos são pontuais
(`MoveFileEx`/`DeleteFile`/`CreateDirectory` para rm/mv/mkdir; `fdatasync`→
`FlushFileBuffers` para sync). Nenhum deles reabre um problema estrutural como
`cat`/`ls` já resolvem.

---

## Resumo executivo

- **Superfície mínima do `cat`**: `openat, read, write, close, mmap-anon,
  exit_group, ioctl(TIOCGWINSZ)-stub` — todas mapeiam limpo para Win32.
- **`ls` adiciona 3 pesos**: `getdents` (via `NtQueryDirectoryFile`), `statx`
  (via `GetFileInformationByHandleEx`, com `st_mode` fabricado) e o
  `ioctl(TIOCGWINSZ)` de verdade.
- **O trabalho difícil não são as syscalls de dados** (essas mapeiam bem) — é o
  **arranque**: `__syscall` dispatcher, `crt0` PE, TLS no modelo NT, e cortar a
  UCRT (o target `x86_64-w64-ntposix`).
- **Permissões Unix** ficam fabricadas (NT não tem o modelo); é aceitável pra v0.
- `fork` e ioctls de terminal **não** estão no caminho de `cat`/`ls`.
