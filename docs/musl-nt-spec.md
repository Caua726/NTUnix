# musl-nt — especificação de implementação

Guia técnico para portar a **musl 1.2.6** para rodar sobre o kernel Windows NT
(Win32 como backend principal, ntdll onde o Win32 não expõe a semântica). É a
libc própria do NTUnix — independente da UCRT da Microsoft.

Baseado na medição real (BusyBox 1.36.1 + musl 1.2.6 oficial); dados brutos em
`docs/pesquisa/`.

## Estado da implementação

O v0 descrito neste documento já possui uma implementação executável em
`musl-nt/`:

- a musl 1.2.6 é compilada com tipos LP64 e empacotada em
  `build/libc-nt.a`;
- `crt0.o` cria `argc`/`argv`/`envp` UTF-8 e um auxv mínimo sem usar CRT da
  Microsoft;
- `ntposix-gcc` compila aplicações e gera PE/COFF sem UCRT ou MSVCRT;
- o backend cobre arquivos, diretórios, metadata/statfs, memória, tempo, links,
  sockets Winsock, identidade sintética e processo básico;
- um BusyBox com mais de 80 applets de coreutils compila sem alteração no código-fonte
  e passa pela bateria `musl-nt/test/busybox-runtime.cmd` sob Wine.

As seções abaixo mantêm o dimensionamento original e documentam também as
decisões tomadas durante a implementação. A fonte da musl continua externa ao
repositório.

---

## 1. Estratégia (por que isso é viável)

A musl fala com o kernel por **um único ponto**: `__syscall(n, ...)`. Todo
`open`/`read`/`stat` no fim vira `__syscall(SYS_xxx, ...)`. Portanto:

> **Reescrevemos o `__syscall` e a camada de arranque; o resto da musl —
> stdio, malloc, printf, string, regex, time — roda intacto.**

**Dimensionamento (medido):** o BusyBox coreutils consome **242 funções** da
musl (~10 mil linhas, **16,6% do source**). Dessas 10k linhas, **reescrevemos
quase nada** — só a fina camada de OS abaixo do `__syscall`. Os módulos pesados
(`stdio` 4.4k, `regex` 4.3k, `string`, `malloc`, `time`) são reusados como estão.

```
  Aplicação (busybox)                     ← compila sem alteração
        │  chama open()/read()/printf()
  musl 1.2.6  (stdio, string, malloc,     ← REUSADA INTACTA (~90%)
        │      printf, regex, time...)
        │  __syscall(SYS_xxx, args)
  ┌─────┴──────────────────────────────┐
  │  MUSL-NT  (o que a gente escreve):  │  ← A CAMADA A ESCREVER
  │   • crt0 PE + __libc_start_main+TLS │
  │   • __nt_syscall  (o dispatcher)    │
  │   • backend por syscall → Win32/ntdll│
  │   • fd-table, errno-xlat, statx/dents│
  └─────┬──────────────────────────────┘
        │  CreateFile / ReadFile / NtQueryDirectoryFile ...
  ntdll + kernel32  (fundação NT preservada — VISÃO §3)
        │
  kernel NT
```

---

## 2. Componentes a escrever (a checklist)

### 2.1 Toolchain / build — target `x86_64-w64-ntposix`
- Compilar a musl 1.2.6 com Clang usando a ABI LP64 de
  `x86_64-unknown-linux-musl`, emitir LLVM bitcode e baixar para PE/COFF.
- Marcar explicitamente funções e chamadas como `X86_64_SysV` antes do `llc`;
  isso preserva `long` de 64 bits e o `va_list` System V, que são incompatíveis
  com o LLP64/Win64 nativo do MinGW.
- Compilar apenas o backend `nt/*.c` com MinGW e cruzar a fronteira por
  `__nt_syscall` com atributo `sysv_abi`.
- `-nostdlib -nodefaultlibs`: **cortar a UCRT** — nenhum `api-ms-win-crt-*`.
- Linkar apps contra: nosso `crt0.o` + `libc.a` (musl+backend) + `libgcc`.
- Entregável: um wrapper `ntposix-gcc` que faz isso automaticamente.
- O rewriter de IR está em `tools/sysv-coff-rewrite.cpp`; a compilação de uma
  unidade LP64 está em `tools/lp64-coff-cc`.

### 2.2 Arranque (o `crt`)
- `crt0` PE (`_start` ou `mainCRTStartup`): obter linha de comando
  (`GetCommandLineW`) e ambiente (`GetEnvironmentStringsW`), converter para
  `argc/argv[]/envp[]` UTF-8, e chamar `__libc_start_main`.
- `__libc_start_main` (musl): reusável; depende de `__init_tls` e
  `SYS_set_tid_address` (stub).
- **`__init_tls` no modelo NT:** manter o bloco `struct pthread` da musl, mas
  guardar seu thread pointer em um slot de `TlsAlloc`/`TlsGetValue`.

### 2.3 O dispatcher `__nt_syscall`
- Reescrever `arch/x86_64/syscall_arch.h`: os `__syscallN(...)` inline passam a
  chamar `long __nt_syscall(long n, long a1..a6)`.
- `__nt_syscall` = um `switch (n)` que despacha cada `SYS_*` para o backend.
- **Convenção de retorno = a do Linux:** sucesso ≥ 0; erro = `-errno`
  (a musl faz `if (r<0){errno=-r; return -1;}`). Todo backend devolve `-errno`.

### 2.4 Backend por syscall (o trabalho mecânico)
Uma função por syscall, traduzindo args Linux → NT e status → `-errno`.
Amostra das principais; o mapa completo está na §3:

| SYS_* | função | NT |
|---|---|---|
| `openat` | `sys_openat` | `CreateFileW` |
| `read` | `sys_read` | `ReadFile` |
| `write` | `sys_write` | `WriteFile` |
| `writev` | `sys_writev` | N× `WriteFile` |
| `close` | `sys_close` | `CloseHandle` |
| `lseek` | `sys_lseek` | `SetFilePointerEx` |
| `mmap`/`munmap` | `sys_mmap` | `VirtualAlloc`/`VirtualFree` |
| `statx` | `sys_statx` | `GetFileInformationByHandleEx` |
| `getdents64` | `sys_getdents` | `NtQueryDirectoryFile` |
| `ioctl(TIOCGWINSZ)` | `sys_ioctl` | `GetConsoleScreenBufferInfo` |
| `exit_group` | `sys_exit` | `ExitProcess` |

### 2.5 Tradução de erros — `errno`
- Tabela `NTSTATUS`/Win32-error → Linux `errno`
  (`ERROR_FILE_NOT_FOUND`→`ENOENT`, `ERROR_ACCESS_DENIED`→`EACCES`, …).
- É o que faz o retorno `-errno` bater com o que a musl espera.

### 2.6 fd-table (crítico)
- A musl usa **fd inteiros pequenos** (`open` devolve `int`, `read(int,…)`); o NT
  usa `HANDLE` (ponteiro). Precisa de uma tabela `fd ↔ HANDLE`.
  0/1/2 = `GetStdHandle`. A implementação independente e sincronizada está em
  `musl-nt/nt/fdtable.c`.

### 2.7 Estruturas e conversões
- **`struct statx`**: preencher de `GetFileInformationByHandleEx`
  (tamanho/tempos limpos; `st_mode`/`uid`/`gid` **fabricados** — NT não tem o
  modelo Unix; dir=`040755`, arquivo=`0100644`, uid=gid=0).
- **`struct dirent64`** (`d_ino`,`d_type`,`d_name`): de
  `FILE_*DIRECTORY_INFORMATION` do `NtQueryDirectoryFile`.
- **Paths**: `/x` → `%NTUNIX_ROOT%\x`; sem a variável, a raiz é inferida pelo
  diretório do PE (removendo `system\bin` quando aplicável). `AT_FDCWD` e dirfd
  permanecem relativos ao cwd.

---

## 2.8 Árvore de arquivos a escrever

A musl 1.2.6 source vem de fora (não versionada; submodule ou baixada). Tudo
abaixo, sob `musl-nt/`, é código **nosso** — o conjunto completo que precisa
existir para a `libc-nt.a` compilar e linkar. Tudo é escrito de uma vez; não há
faseamento — cada arquivo cobre a sua fatia inteira da ABI.

```
musl-nt/
├── Makefile                     builda musl(externa)+nossos .o → libc-nt.a + crt0.o
├── ntposix-gcc                  wrapper do mingw: -nostdlib -I musl/include -I nt;
│                                linka crt0.o + libc-nt.a + libgcc
├── arch/x86_64/
│   ├── syscall_arch.h  (override) __syscallN(n,…) → __nt_syscall(n,…)
│   ├── pthread_arch.h  (override) thread pointer via TLS do NT
│   └── crt_arch.h      (override) neutraliza o _start Linux da musl
├── crt/
│   └── crt0.c                    _start/mainCRTStartup: GetCommandLineW +
│                                 GetEnvironmentStringsW → argc/argv/envp UTF-8 →
│                                 __libc_start_main
├── override/
│   ├── __init_tls.c              TLS no modelo NT (o gargalo — §5.1)
│   ├── __set_thread_area.c       TlsAlloc/TlsSetValue/TlsGetValue
│   ├── syscall_cp.c              cancelamento cooperativo de syscall
│   └── setjmp.S                  setjmp/longjmp System V em objeto COFF
├── nt/                          ← a camada de OS (o núcleo do trabalho)
│   ├── nt_syscall.c / .h         __nt_syscall(n,a1..a6): o switch(n) dispatcher
│   ├── fdtable.c / .h            tabela int fd ↔ HANDLE (base já em src/libntposix)
│   ├── errno_xlat.c / .h         Win32 error / NTSTATUS → -errno
│   ├── ntpath.c / .h             "/x" → "%NTUNIX_ROOT%\x"; AT_FDCWD/dirfd
│   ├── convert.c / .h            FILETIME↔timespec, struct statx, struct dirent64
│   ├── sys_file.c                openat, read, write, writev, readv, close, lseek,
│   │                             dup, dup2, fcntl, ftruncate, pread/pwrite
│   ├── sys_mem.c                 mmap(anon), munmap, mprotect, brk → VirtualAlloc/Free
│   ├── sys_stat.c                statx, fstat, faccessat → GetFileInformationByHandleEx
│   ├── sys_dir.c                 getdents64 → NtQueryDirectoryFile → dirent64;
│   │                             mkdirat, unlinkat, renameat
│   ├── sys_link.c                readlinkat (reparse points)
│   ├── sys_fs.c                  rename, unlink, mkdir, rmdir, chdir, getcwd, sync,
│   │                             fsync, fdatasync, statfs/fstatfs
│   ├── sys_net.c                 sockets IPv4/IPv6 → Winsock carregada dinamicamente;
│   │                             bind/connect/listen/accept, send/recv, opts e poll
│   ├── sys_proc.c                exit, pid/ppid, execve/wait via CreateProcess;
│   │                             fork/vfork → ENOSYS
│   ├── sys_time.c                clock_gettime, clock_nanosleep, gettimeofday
│   ├── sys_ioctl.c               ioctl(TIOCGWINSZ) → GetConsoleScreenBufferInfo, isatty
│   ├── sys_signal.c              rt_sigaction/rt_sigprocmask (stubs p/ o startup linkar)
│   └── stubs.c                   syscalls só-p/-linkar (getuid/getgid=0, umask,
│                                 getgroups, uname… → valores fixos plausíveis)
├── include/nt/                   ntabi.h (a ABI Linux) + ntpriv.h (protótipos) — os
│                                 nt/*.c incluem só estes, nunca os headers da musl
├── tools/                        compilador LP64→COFF, rewriter LLVM e config BusyBox
└── test/                         hello/smoke/memory/network + miniconfig e bateria BusyBox
```

**Total implementado: 41 arquivos nossos, ~5,9 mil linhas**, além dos arquivos
da musl reusados. As ~10 mil linhas medidas no Apêndice A são a fatia da musl
consumida pelo BusyBox, não código que precisava ser reescrito.

> Nada aqui reimplementa `stdio`/`string`/`malloc`/`printf`/`regex` — esses são
> os `.c` da própria musl, compilados como estão pelo `Makefile`.

---

## 3. Mapa syscall → NT (referência completa)

**Limpo em Win32:** `openat`(CreateFileW), `read`(ReadFile), `write`(WriteFile),
`close`(CloseHandle), `lseek`(SetFilePointerEx), `mmap/munmap`(VirtualAlloc/Free),
`ftruncate`(SetEndOfFile), `fsync`(FlushFileBuffers), `rename`(MoveFileEx),
`unlink`(DeleteFile), `mkdir`(CreateDirectory), `rmdir`(RemoveDirectory),
`chdir`(SetCurrentDirectory), `getcwd`(GetCurrentDirectory),
`clock_gettime`(QueryPerformanceCounter/GetSystemTimePreciseAsFileTime),
`nanosleep`(Sleep), `getpid`(GetCurrentProcessId), `exit_group`(ExitProcess),
`isatty`/`ioctl(TIOCGWINSZ)`(GetFileType/GetConsoleScreenBufferInfo),
`socket`/`bind`/`connect`/`accept`/`send`/`recv` (Winsock).

**Melhor via ntdll:** `getdents64`(NtQueryDirectoryFile — várias entradas num
buffer, casa com getdents), `statx`(NtQueryInformationFile p/ detalhe fino),
`readlinkat`(DeviceIoControl FSCTL_GET_REPARSE_POINT).

**Problemáticos / fabricar / adiar:** permissões Unix (`chmod`/`chown`/`umask` —
NT usa ACL; virar no-op ou emulação mínima); `fork` (**hostil ao NT, fora do
caminho do coreutils**; `execve` usa `CreateProcessW`); ioctls de
terminal raw (`TCGETS/TCSETS` — stty/less, não coreutils); `/proc` (não coreutils).

---

## 4. Ordem de dependência de compilação

Não há faseamento de features — a camada é escrita e compilada inteira. A ordem
abaixo é só a de **dependência entre os arquivos** (o que precisa existir antes
do que, para compilar e linkar limpo):

1. **`include/nt/ntabi.h`** — a ABI Linux (números de syscall, structs, flags,
   errno). Base de todos os `nt/*.c`; não depende de nada.
2. **`include/nt/ntpriv.h`** — junta `<windows.h>` + `ntabi.h` e declara os
   helpers e todos os `nt_sys_*`.
3. **Helpers** — `errno_xlat.c`, `fdtable.c`, `ntpath.c`, `convert.c`. Puros,
   sem depender dos backends.
4. **Backends** — `sys_file.c`, `sys_mem.c`, `sys_stat.c`, `sys_dir.c`,
   `sys_link.c`, `sys_fs.c`, `sys_proc.c`, `sys_time.c`, `sys_ioctl.c`,
   `sys_signal.c`, `sys_net.c`, `stubs.c`. Cada um usa os helpers do passo 3.
5. **`nt/nt_syscall.c`** — o `switch(n)` que chama todos os backends do passo 4.
6. **Arranque** — `override/__set_thread_area.c` (slot TLS do NT),
   `override/__init_tls.c` (TLS sem ELF), `crt/crt0.c`
   (`mainCRTStartup` → `__libc_start_main`).
7. **Overrides de arch** — `arch/x86_64/syscall_arch.h` (redireciona o
   `__syscall`), `arch/x86_64/crt_arch.h` (neutraliza o `_start` da musl).
8. **Build** — `Makefile` compila a árvore C da musl (o linker elimina o que
   não é alcançado) + tudo acima em `libc-nt.a` + `crt0.o`; `ntposix-gcc`
   linka um app contra eles.

Validação implementada: `make musl-nt-test` executa os testes de libc, memória,
statfs, UDP/TCP loopback, mensagens vetorizadas e resolução por `/etc/hosts`,
além de verificar imports; `make busybox-nt-test` configura, compila e executa
o BusyBox coreutils inteiro, rejeitando UCRT/MSVCRT.

---

## 5. Riscos e decisões tomadas

1. **TLS.** Resolvido com `TlsAlloc`/`TlsGetValue`, sem tomar `%fs`. Isso é
   compatível tanto com Windows real quanto com Wine, que já usa os registradores
   de segmento para seu próprio runtime.
2. **Compilar a musl como PE.** Resolvido pelo pipeline LP64/System V → LLVM IR
   reescrito → COFF. Compilar diretamente com MinGW/LLP64 foi descartado porque
   quebra layouts e funções variádicas.
3. **Permissões Unix fabricadas.** `ls -l` mostra `0644`/`0755` fixos; `chmod` é
   no-op. Aceitável p/ v0; modelo real (ACL↔mode) fica pós-v0.
4. **Symlinks.** Implementados por reparse points e
   `FSCTL_GET_REPARSE_POINT`; criação ainda depende da política de privilégios
   do host.
5. **`fork`.** Não implementar genérico (WSL1-style precisa de kernel). Não está
   no caminho do coreutils. `execve` cria e aguarda um processo NT; uma
   implementação nativa completa de `posix_spawn` fica pós-v0.
6. **Winsock sem colisão de ABI.** A import library `ws2_32` exporta nomes como
   `bind`, `connect`, `send` e `recv`, iguais aos símbolos públicos da musl.
   O backend carrega `ws2_32.dll` e resolve as entradas com `GetProcAddress`,
   mantendo todas as chamadas sob nomes privados. IPv4/IPv6, stream/datagram,
   opções, poll, duplicação de descritores e `socketpair` sobre loopback estão
   funcionais; SCM/ancillary data e descoberta automática de DNS permanecem
   pós-v0.

---

## 6. Referências (dados brutos)

- `docs/pesquisa/busybox-coreutils-musl-deps.md` — syscalls por applet, mapa NT.
- `docs/pesquisa/nt-native-api.md` — Native API / ntdll (ordem de ataque).
- `docs/pesquisa/bb-coreutils-funcoes-libc-consumidas.txt` — as 242 funções.
- `docs/pesquisa/bb-coreutils-headers-publicos.txt` — os 85 headers públicos
  (a musl fornece 66; os 19 ausentes nenhum coreutils inclui).
- `docs/pesquisa/bb-coreutils-headers-fecho-completo.txt` — os 477 headers do
  fecho transitivo de compilação.
- Fontes de referência: musl 1.2.6 (`/tmp/musl-1.2.6`), BusyBox 1.36.1
  (`/tmp/bbsrc`), musl instalada (`/usr/lib/musl`, `musl-gcc`).

---

## Apêndice A — os 196 arquivos da musl medidos

Os arquivos da musl 1.2.6 que implementam as 242 funções que o conjunto
BusyBox **coreutils + libbb** referencia (medido, nível direto). A
implementação atual compila uma biblioteca mais ampla a partir da árvore da
musl e usa `--gc-sections`; esta lista continua sendo a medição do fecho
consumido pelo BusyBox.

> **A maioria compila sem alteração** — é o "resto da musl" reusado. Exceções
> que precisam de override/stub na camada `nt/` (marcadas 🔶): `process/fork.c`,
> `vfork.c` (fork não-genérico, §5.5), os 20 de `network/` (sockets — vêm do
> `libbb` genérico, não do coreutils puro) e `linux/*` (Linux-específicos). Num
> build **só-coreutils** com `--gc-sections`, boa parte de `network/` e `fork`
> nem entra. O fecho transitivo interno (helpers `__stdio_write`, `__lockfile`,
> `__syscall_ret`…) entra automaticamente na build.

### `src/conf/` (1)
sysconf.c

### `src/dirent/` (2)
closedir.c opendir.c

### `src/env/` (6)
clearenv.c getenv.c putenv.c setenv.c __stack_chk_fail.c unsetenv.c

### `src/errno/` (2)
__errno_location.c strerror.c

### `src/exit/` (1)
exit.c

### `src/legacy/` (1)
utmpx.c

### `src/linux/` (6) 🔶
cap.c chroot.c prctl.c settimeofday.c syncfs.c utimes.c

### `src/malloc/` (3)
free.c oldmalloc/malloc.c realloc.c

### `src/misc/` (12)
dirname.c gethostid.c getopt.c getopt_long.c getpriority.c ioctl.c mntent.c pty.c realpath.c setpriority.c syslog.c uname.c

### `src/network/` (20) 🔶
bind.c connect.c freeaddrinfo.c getaddrinfo.c gethostbyname.c getnameinfo.c getpeername.c getservbyname.c getsockname.c h_errno.c hstrerror.c inet_aton.c inet_ntoa.c inet_pton.c listen.c recvmsg.c sendmsg.c sendto.c setsockopt.c socket.c

### `src/prng/` (1)
rand.c

### `src/process/` (6) 🔶
execv.c execvp.c fork.c vfork.c wait.c waitpid.c

### `src/regex/` (4)
fnmatch.c regcomp.c regerror.c regexec.c

### `src/sched/` (1)
affinity.c

### `src/select/` (1)
poll.c

### `src/setjmp/` (1)
longjmp.c

### `src/signal/` (11)
kill.c raise.c sigaction.c sigaddset.c sigemptyset.c sigfillset.c signal.c sigprocmask.c sigrtmax.c sigrtmin.c sigsuspend.c

### `src/stat/` (8)
chmod.c fchmod.c futimens.c mkdir.c mkfifo.c mknod.c umask.c utimensat.c

### `src/stdio/` (21)
clearerr.c dprintf.c fclose.c fflush.c fprintf.c fputc.c fputs.c fread.c fwrite.c getc_unlocked.c getline.c printf.c putchar_unlocked.c putc_unlocked.c puts.c rename.c setbuf.c snprintf.c sprintf.c vasprintf.c vsnprintf.c

### `src/stdlib/` (4)
atof.c atoi.c qsort.c strtod.c

### `src/string/` (23)
memchr.c memcmp.c memmove.c stpcpy.c strcasecmp.c strcasestr.c strcat.c strchr.c strchrnul.c strcmp.c strcpy.c strcspn.c strdup.c strlen.c strncasecmp.c strncmp.c strncpy.c strndup.c strrchr.c strsep.c strspn.c strstr.c strverscmp.c

### `src/temp/` (2)
mkdtemp.c mktemp.c

### `src/termios/` (5)
cfgetospeed.c cfsetospeed.c tcflush.c tcgetattr.c tcsetattr.c

### `src/time/` (12)
clock_gettime.c clock_settime.c ctime.c gettimeofday.c localtime.c localtime_r.c mktime.c nanosleep.c strftime.c strptime.c time.c timegm.c

### `src/unistd/` (42)
access.c alarm.c chdir.c chown.c close.c dup2.c dup.c _exit.c fchdir.c fchown.c fdatasync.c fsync.c getcwd.c getegid.c geteuid.c getgid.c getgroups.c getlogin_r.c getpid.c getppid.c getuid.c isatty.c lchown.c link.c pipe.c read.c readlink.c rmdir.c setegid.c seteuid.c setgid.c setresgid.c setresuid.c setsid.c setuid.c sleep.c symlink.c sync.c ttyname_r.c unlink.c usleep.c write.c
