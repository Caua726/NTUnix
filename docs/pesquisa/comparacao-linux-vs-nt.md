# Comparação syscall-a-syscall: kernel Linux vs backend NT (musl-nt)

> Fonte primária: código do backend em `musl-nt/nt/*`, `musl-nt/override/*`, `musl-nt/crt/crt0.c`.
> Semântica Linux: man-pages (fork(2), execve(2), pipe(7), read(2), open(2), dup(2),
> mmap(2), signal(7)) e `fs/read_write.c`/`kernel/fork.c` do kernel.
> Semântica NT: docs MS (ReadFile, CreateProcess, CreatePipe, NtQueryDirectoryFile) e,
> para o clone (RtlCloneUserProcess, **sem doc oficial MS**), a pesquisa de referência
> "The Definitive Guide To Process Cloning on Windows" (huntandhackett/diversenok).

Legenda de risco: 🟢 fiel · 🟡 aproximação com bordas · 🔴 divergência que quebra programas.

---

## 0. O modelo geral

| | Linux | NTUnix (musl-nt) |
|---|---|---|
| Fronteira | `syscall` → kernel monolítico | `__nt_syscall(n,...)` → `switch(n)` → Win32/ntdll (`nt/nt_syscall.c`) |
| Tabela de fds | `struct files_struct` no kernel; fd→`struct file` | array estático `fd_table[]` em userspace (`nt/fdtable.c`), fd→`HANDLE` |
| Posição de arquivo | no `struct file` (open file description), **compartilhada** por fds duplicados/herdados | no `FILE_OBJECT` do NT (CurrentByteOffset) — semântica de compartilhamento parecida em handles herdados |
| Processo | `task_struct`; fork duplica o kernel-side atômico | processo Win32; fork = **clone do processo inteiro** (`RtlCloneUserProcess`) |
| Thread | `clone(CLONE_VM|CLONE_THREAD)` | **inexistente** — não há `SYS_clone`; `pthread_create`→ENOSYS |

Consequência de projeto que atravessa tudo: **o userland (musl+busybox) roda INTEIRO dentro
do clone parcial** depois de um `fork()`. O NT não suporta isso como ambiente de execução
geral — a própria pesquisa de referência diz que "a maioria dos programas não roda mais que
0,1 ms antes de crashar ou sair" num clone, e que "funcionar ou crashar depende das
facilities do SO que o código usa". É a raiz do bug do `cat`/pipeline (§4).

---

## 1. Processo e exec

### 1.1 `fork` / `vfork` — `nt_sys_fork` (`sys_proc.c:293`) 🔴
- **Linux**: duplicação atômica no kernel. Filho é um processo normal completo: mesma
  imagem, cópia COW da memória, cópia da fd table com open file descriptions
  **compartilhadas**, TLS (FS_BASE) copiado. Tudo que o pai fazia, o filho faz.
- **NT**: `RtlCloneUserProcess(CREATE_SUSPENDED|INHERIT_HANDLES)`. Retorna
  `STATUS_PROCESS_CLONED (0x129)` no filho. O filho **reanexa o console**
  (`FreeConsole`+`AttachConsole(ATTACH_PARENT_PROCESS)`+`nt_fd_reattach_console`,
  `sys_proc.c:316-327`) e continua executando código musl/busybox dentro do clone.
- **Divergências**:
  - No clone, só a thread chamadora é clonada; a conexão com o CSR/conhost é
    refeita à mão. Facilities do SO ficam num estado "indefinido": algumas funcionam
    (`WriteFile`, `NtQueryDirectoryFile` — por isso `ls` e o logger funcionam), outras
    não (`ReadFile` de arquivo retorna 0 — o bug, §4).
  - Ausente no Wine (`clone_fn==0` → `-ENOSYS`, `sys_proc.c:307`).
  - `nt_fd_make_inheritable()` marca **todos** os handles como herdáveis antes do
    clone (`fdtable.c:163`). Coincide com Linux (fds atravessam o fork
    independentemente de CLOEXEC), mas é grosso: sockets são pulados → **um fork com
    socket aberto perde o socket no filho** 🔴.

### 1.2 `execve` — `nt_sys_execve` (`sys_proc.c:170`) 🔴 (vários)
- **Linux**: substitui a imagem do processo **no mesmo pid**; fds sem CLOEXEC
  sobrevivem; retorna só em erro.
- **NT**: `CreateProcessW(...)` + `WaitForSingleObject` + `ExitProcess(exit_code)`
  (`sys_proc.c:203-216`). Ou seja, **spawn-e-espera**, não substituição. Problemas:
  1. **pid muda** através do exec (Linux preserva). Quebra `$$`, pidfiles, `kill` por pid,
     job control. 🔴
  2. **Sem `STARTF_USESTDHANDLES`** (`STARTUPINFOW` zerada, `sys_proc.c:200-204`): o
     processo criado **não recebe fd 0/1/2 redirecionados** (pipes/arquivos). Só herda o
     console por padrão. Logo `cmd > arq`, `a | b`, `cmd < arq` **perdem o stdio** quando
     o alvo é um binário externo de verdade. 🔴
  3. **A fd table não é serializada**: só há herança acidental de handles pelo mesmo
     valor numérico (bInheritHandles=TRUE), mas fds >2 não são comunicados nem
     necessariamente herdáveis. Em Linux, todo fd sem CLOEXEC sobrevive ao exec. 🔴
  4. Como espera e re-sai, um `fork()+execve()` cria **duas camadas** (o clone vira um
     "wait wrapper" do processo real). O `wait4` do shell vê o pid do clone, não o do
     binário.
  5. CLOEXEC não é aplicado no exec (não há fechamento de fds marcados).

### 1.3 `wait4` — `nt_sys_wait4` (`sys_proc.c:220`) 🟡
- Só espera filhos **registrados por `fork`** (tabela `g_children`). Filhos vindos do
  `CreateProcessW` do `execve` **não são registrados** → não são "esperáveis". 🔴
- Só a option `WNOHANG` (`options & ~1` → EINVAL). Sem `WUNTRACED`/`WCONTINUED`.
- Status = `(exit_code & 0xff) << 8` — só saída normal; **sem codificação de sinal**
  (`WIFSIGNALED`/`WTERMSIG` sempre falso). `rusage` ignorado.
- `WaitForMultipleObjects` limita a `MAXIMUM_WAIT_OBJECTS` (64) filhos simultâneos.

### 1.4 `kill` — `nt_sys_kill` (`sys_proc.c:269`) 🔴
- Só `sig==0` (existência, via OpenProcess), `9` e `15` → **`TerminateProcess`**
  (não-capturável). Qualquer outro sinal → ENOSYS. `pid<=0` (grupos) → ENOSYS.
- **Linux**: entrega sinal ao handler; SIGTERM capturável, SIGKILL não; `kill(-pgrp)`.
  Aqui SIGTERM **não pode ser capturado** (é TerminateProcess) → cleanup handlers não rodam.

### 1.5 Identidade / sessões (`nt_syscall.c:122-126`) 🔴
- `getpid`/`getppid` reais (via NT). Mas `getpgrp/setsid/getpgid/getsid` **fingem** devolver
  o pid. Não há grupos de processo nem sessões → **sem job control real**, `killpg`,
  terminal de controle. Ctrl-C não vira SIGINT para um grupo.

---

## 2. Threads, TLS, cancelamento, futex

### 2.1 Criação de thread — **ausente** 🔴
- Não há `case SYS_clone`. `pthread_create` → default → `nt_sys_stub` → **ENOSYS**.
  **O userland é single-thread.** (Efeito colateral bom: o problema clássico de "fork em
  processo multithread com lock preso" não acontece — só há uma thread no fork.)

### 2.2 Thread pointer / TLS — `override/__set_thread_area.c`, `__init_tls.c` 🟡
- O ponteiro de thread (usado por errno, malloc, stdio) fica num **slot TLS do NT**
  (`TlsSetValue`/`TlsGetValue`), não no `%fs`base. Funciona no clone (o `ls` precisa de
  malloc+errno e funciona). `__copy_tls`/`__init_tp` seguem o desenho da musl.

### 2.3 Cancelamento — `override/syscall_cp.c` 🟡
- **Cooperativo apenas**: o wrapper lê a palavra de cancelamento e retorna `-EINTR`
  ANTES de entrar no backend; um `read()`/`write()` **já bloqueado não é interrompido**.
- **Linux**: sinal chega, a syscall retorna EINTR de dentro. Aqui não há entrega de
  sinal, então `read()` bloqueante não dá EINTR → Ctrl-C durante leitura não corta.

### 2.4 `futex` — `nt_sys_futex` (`stubs.c:117`) 🟡
- Só `FUTEX_WAIT`/`FUTEX_WAKE` via `WaitOnAddress`/`WakeByAddress*`. Sem REQUEUE, PI,
  bitset. Suficiente para locks de um único processo single-thread.

---

## 3. Descritores e tabela de handles

### 3.1 `open`/`openat` — `sys_file.c:37` 🟡
- `CreateFileW` com share R/W/D total, `FILE_FLAG_BACKUP_SEMANTICS` sempre (necessário
  p/ abrir diretório; inócuo p/ arquivo). Mapeia O_ACCMODE/APPEND/CREAT/EXCL/TRUNC.
- `mode`/umask **ignorados** (`(void)mode`) — arquivo novo não recebe permissão Unix. 🟡
- O_NONBLOCK aceito mas sem efeito em arquivo/pipe (§5). Sem O_DIRECT/O_SYNC.

### 3.2 `close`/`dup`/`dup2`/`dup3`/`fcntl` — `sys_file.c`, `fdtable.c` 🟡
- `dup*` via `DuplicateHandle` (ou `nt_socket_duplicate`). CLOEXEC ↔ `HANDLE_FLAG_INHERIT`.
- **`dup2`/`dup3` NÃO chamam `SetStdHandle`** (`fdtable.c:215`). Como todo I/O passa pela
  `fd_table` (não por `GetStdHandle`), é consistente **internamente**. Mas um redirect de
  shell `cmd >arq` feito como `open`+`dup2(fd,1)`+`execve` **não chega** ao binário: nem
  a `fd_table` nem `GetStdHandle` são propagados pelo `CreateProcessW` (falta
  `STARTF_USESTDHANDLES`, §1.2). 🔴
- `fcntl`: F_DUPFD/CLOEXEC, F_GETFD/SETFD, F_GETFL/SETFL (só APPEND|NONBLOCK). **Sem
  F_SETLK/GETLK** (locking de arquivo POSIX) → programas que travam arquivo não travam. 🟡

### 3.3 Herança no fork/exec 🔴
- Fork: `make_inheritable` liga inherit em tudo (menos socket). OK para fork.
- Exec: não há transporte da fd table → só fds 0/1/2 e por acidente (§1.2).

---

## 4. I/O de arquivo — **o bug**

### 4.1 `read`/`write` — `transfer_read`/`transfer_write` (`sys_file.c:66`,`96`) 🔴 no clone
- **Linux**: `read` usa `file->f_pos`; arquivo recém-aberto começa em 0; retorno 0 = EOF
  (`fs/read_write.c`). `cat FILE` = loop `read()`+`write()` (confirmado no fonte do
  busybox `bb_full_fd_action` e do coreutils `simple_cat`); `read()==0` → sem saída.
- **NT**: `ReadFile`/`WriteFile` síncronos. Trata `ERROR_BROKEN_PIPE`/`ERROR_HANDLE_EOF`
  como 0. Console cooked remove `\r` (emula ICRNL). O código do read path está **correto**
  contra a doc da MS.
- **O DEFEITO**: dentro do clone (`fork`), `ReadFile` num arquivo de disco **retorna
  0 bytes** (EOF espúrio), enquanto `CreateFileW`+`WriteFile`+`CloseHandle` funcionam (o
  logger `fl_raw` prova). Por isso:
  - `cat /fork.log` (applet forkeado, standalone) → `read()==0` → **nada, exit 0**, sem
    erro (se fosse erro, o busybox imprimiria "read error" no stderr).
  - `ls / | wc -l` → o estágio que **lê** (pipe/arquivo) não recebe dados / não recebe EOF.
- **CORREÇÃO empírica (probe rodado no VM, 2026-07-17):** a hipótese "read de arquivo
  devolve 0 no clone" foi **REFUTADA**. O probe nativo dentro do clone leu 64 bytes de
  `/fork.log` com sucesso (`C-rdok=1`, `C-rdcnt=0x40`). **Leitura de arquivo FUNCIONA no
  clone.** O que **FALHA** é `CreatePipe` dentro do clone: `C-pipe=6` =
  `ERROR_INVALID_HANDLE`. Ou seja, o culpado do `echo | wc` (e pipelines) é a camada de
  **pipe/handle no clone**, não o `read`. Ver §16.

### 4.2 `pread`/`pwrite`/`preadv`/`pwritev` — `positional_io` (`sys_file.c:274`) 🟡
- **Linux**: atômicos, **não** tocam o offset compartilhado.
- **NT**: salva posição → `SetFilePointerEx` → I/O → restaura, sob `io_lock`. Atômico
  **dentro do processo**, mas move o offset do `FILE_OBJECT`: num handle herdado/compartilhado
  entre pai e filho, há **corrida de posição** cross-process. 🔴

### 4.3 `readv`/`writev` — `sys_file.c:140` 🟡
- Loop de `read`/`write` por iovec — **não** é vetor atômico como no Linux; escrita parcial
  pode intercalar no meio do vetor.

### 4.4 `lseek`/`ftruncate`/`fsync` 🟢/🟡
- `lseek` em pipe/console → ESPIPE (correto). `ftruncate` salva/seek/SetEndOfFile/restaura.
- `fsync` em O_RDONLY: Win32 exige GENERIC_WRITE p/ `FlushFileBuffers`, então reabre p/
  escrita só pra dar flush (`sys_fs.c:110`) — engenhoso. `sync()` é no-op (advisory).

---

## 5. Pipes — `nt_sys_pipe2` (`sys_file.c:316`) 🔴
- **Linux**: pipe suporta O_NONBLOCK (read→EAGAIN), EOF quando todas as pontas de escrita
  fecham, escrita ≤PIPE_BUF atômica.
- **NT**: `CreatePipe` = anônimo, byte-mode, **só bloqueante**. `O_NONBLOCK` é **aceito e
  ignorado** (`sys_file.c:322`,`334`) → um programa que faz o pipe não-bloqueante e espera
  EAGAIN vai **bloquear**. 🔴
- `poll`/`ppoll` (`sys_file.c:351`): POLLIN via `PeekNamedPipe`, POLLHUP via
  `ERROR_BROKEN_PIPE`. **POLLOUT sempre ligado** (não detecta pipe cheio) 🟡. Faz busy-poll
  com `Sleep(1)`.
- EOF depende de fechar todas as pontas de escrita; com `make_inheritable` grosso, uma
  ponta de escrita herdada a mais pode **atrasar/impedir o EOF** → risco de pipeline
  travado (além do bug de leitura no clone).

---

## 6. Diretórios — `nt_sys_getdents64` (`sys_dir.c:30`) 🟡
- `NtQueryDirectoryFile(FileIdBothDirectoryInformation, ReturnSingleEntry=TRUE)` — uma
  entrada por chamada (ineficiente, mas **funciona no clone**, por isso `ls` anda).
- Posição do diretório em userspace (`dir_cookie`/`dir_eof` no slot). `d_ino`=FileId,
  `d_off`=cookie sintético (não seekável → `seekdir` real não funciona). `d_type` derivado
  de atributos. NTFS já devolve `.`/`..`.
- mkdir/unlink/rmdir/rename/link/symlink/readlink em `sys_dir.c`/`sys_link.c` via Win32
  (`CreateDirectoryW`, `DeleteFileW`, `MoveFileExW`, `CreateHardLinkW`,
  `CreateSymbolicLinkW`, reparse points). `renameat2` só suporta NOREPLACE. 🟡

---

## 7. Metadata e identidade

### 7.1 `stat`/`statx`/`fstat` — `sys_stat.c` 🟡
- Abre com `FILE_READ_ATTRIBUTES` e converte. `st_ino`=FileId. `st_mode`/uid/gid
  **sintéticos** (uid/gid sempre 0; sem permissão real).

### 7.2 `access`/`faccessat` — `sys_stat.c:87` 🔴 no X_OK
- **X_OK aproximado por extensão** (`.exe/.com/.bat/.cmd`; `sys_stat.c:101`): um script/ELF
  Unix executável **sem extensão** reporta "não executável". W_OK só olha o atributo
  READONLY.

### 7.3 uid/gid/chmod/chown — `stubs.c:159-200` 🟡
- get*id → 0 (root). set*id só aceita 0/-1. chmod/chown/fchmodat → **no-op sucesso**.
  umask guardado mas **não aplicado** no open. Sem modelo de permissão.

### 7.4 `statfs`/`fstatfs` — `sys_fs.c` 🟡 (mostra magic de NTFS, espaço real).

---

## 8. Paths e /dev, /proc — `ntpath.c` 🔴 (lacunas)
- `/x` → `NTUNIX_ROOT` (ex.: `X:\NTUnix`) + conversão `/`→`\`. Absolutos Windows (`C:`,
  `\\`) passam direto. Sem VFS/mount real.
- **`/dev` inexistente**: `/dev/null`, `/dev/zero`, `/dev/tty`, `/dev/urandom` → viram
  `X:\NTUnix\dev\null` (não existe) → ENOENT. Foi o que quebrou `true & wait`
  (`can't open '/dev/null'`). 🔴
- `/proc` inexistente (embora o repo tenha um dir `proc/`). Case-insensitive (NT) vs
  case-sensitive (Linux). 🟡

---

## 9. Memória — `sys_mem.c`
- `mmap` anon → `VirtualAlloc`; arquivo → `CreateFileMapping`+`MapViewOfFileEx`. 🟡
  - **MAP_FIXED** não força de verdade: pede endereço e checa; se não bateu, `EEXIST`
    (não substitui mapeamento existente como o Linux). 🔴
  - Offset de arquivo deve ser múltiplo da **granularidade de alocação (64 KB)**, não da
    página (4 KB) do Linux → `EINVAL` para offset 4K-alinhado mas não 64K. 🔴
- `munmap` de sub-região anônima usa `MEM_DECOMMIT` (mantém a reserva) — não devolve o
  espaço como o Linux; vazamento de address space ao longo do tempo. 🟡
- `brk` sobre região reservada de 256 MB (`sys_mem.c:136`). `mprotect`/`msync`/`madvise`
  (só MADV_DONTNEED→DiscardVirtualMemory) parciais. 🟡

---

## 10. Sinais — `sys_signal.c` 🔴
- `rt_sigaction`/`rt_sigprocmask` **guardam estado mas não há ENTREGA de sinal**. Sem
  SIGCHLD, SIGINT, SIGPIPE, SIGALRM. `alarm`→no-op. `rt_sigsuspend`→EINTR imediato.
- Efeitos: escrita em pipe quebrado retorna **EPIPE** (erro) em vez de matar por SIGPIPE;
  Ctrl-C não vira SIGINT; reaping por SIGCHLD não dispara (o shell tem que dar `wait`).

---

## 11. Tempo — `sys_time.c` 🟢
- `clock_gettime` (REALTIME=FILETIME preciso, MONOTONIC=QPC, CPUTIME=Get*Times),
  `clock_nanosleep` via waitable timer, `gettimeofday`. Sólido. `clock_settime` seta a
  hora do sistema (privilegiado).

---

## 12. Sockets — `sys_net.c` (942 linhas) 🟡
- Backend Winsock completo (socket/bind/listen/accept4/connect/send*/recv*/getsockopt/
  socketpair/shutdown). Fds de socket são um `kind` separado. **Não herdados no fork**
  (make_inheritable pula socket) → um servidor que faz fork por conexão perde o socket. 🔴

---

## 13. termios / ioctl — `sys_ioctl.c` 🟡
- termios mapeado nos modos de console: ICANON↔ENABLE_LINE_INPUT, ECHO↔ENABLE_ECHO_INPUT,
  ISIG↔ENABLE_PROCESSED_INPUT. TIOCGWINSZ do screen buffer. FIONREAD via
  PeekNamedPipe/console events. Sem baud, sem timers VMIN/VTIME reais, c_cc parcial.

---

## 14. Síntese — o que quebra o `cat`/pipeline e o que corrigir

**Raiz única (§0, §1.1, §4):** o userland roda dentro de um clone parcial
(`RtlCloneUserProcess`), ambiente que o NT não suporta para execução geral. Escrita,
getdents e abertura/append de arquivo funcionam; **leitura de arquivo retorna 0**. Como
todo comando que lê arquivo (cat, wc, sort, grep…) é applet forkeado no busybox standalone,
o sintoma é "sem saída".

**Divergências independentes do clone (deriváveis, sem VM):**
1. `execve` sem `STARTF_USESTDHANDLES` → pipelines/redirects para binário externo perdem
   stdio (`sys_proc.c:200`). 🔴
2. `execve` troca o pid e é spawn-e-espera, não substituição. 🔴
3. `/dev/null` (e todo /dev) inexistente → ENOSYS/ENOENT (`true & wait`). 🔴
4. Pipe O_NONBLOCK ignorado (`sys_file.c:322`). 🔴
5. Sockets não atravessam o fork. 🔴
6. Sem entrega de sinal (SIGPIPE/SIGINT/SIGCHLD). 🔴

---

## 15. Estado da implementação (aplicado e validado)

Corrigido no backend, **compilado com mingw e validado rodando a suíte busybox no Wine
(60+ applets: PASS) + testes dirigidos**:

1. **`execve` com `STARTF_USESTDHANDLES`** (`sys_proc.c`): liga fd 0/1/2 nativos ao
   `CreateProcessW`, **só quando há redirecionamento real** (algum de 0/1/2 é pipe/arquivo/
   socket) — se os três são console, mantém a herança automática que já funcionava (sem
   regressão). Resolve redirect/pipeline quando o alvo é processo real. 🟢
2. **devfs** (`sys_file.c` `nt_dev_open`): `/dev/null`→NUL, `/dev/zero`/`/dev/full`/
   `/dev/random`/`/dev/urandom` (sintéticos via campo `devkind`), `/dev/tty`/`/dev/console`
   →CONIN$/CONOUT$, `/dev/stdin|stdout|stderr` e `/dev/fd/N`→dup. Interceptado ANTES da
   resolução de path. Testado no Wine: null (open+write+EOF), full (ENOSPC), fd/0. 🟢
3. **Propagação de `devkind` em dup/dup2** (`fdtable.c`): bug pego no teste — o shell faz
   `dup2` do fd de dispositivo para 0/1/2, e o marcador se perdia; agora é copiado. 🟢
4. **Pipe `O_NONBLOCK` na leitura** (`sys_file.c`): `PeekNamedPipe` antes do `ReadFile`;
   sem dados e sem EOF → EAGAIN. (compilado; não testável no Wine por depender de fork.) 🟢
5. **Probe de diagnóstico** (`sys_proc.c` `fl_probe_read`): no ramo-filho do clone, abre e
   lê `/fork.log` com chamada nativa e loga o resultado. Prova em 1 execução no VM se
   `ReadFile` de arquivo retorna 0 no clone.

Descoberta que corrobora o diagnóstico: o `busybox-runtime.cmd` invoca cada applet como
`busybox.exe <applet>` **direto** (processo principal, sem fork) — por isso `cat` sempre
passou no teste. **Leitura de arquivo funciona em processo normal; só falha no clone.**

### Pendente (decisão de userland + confirmação no VM)
- **Causa raiz do `cat`/pipeline forkeado (estrutural):** rotear applets pesados por
  processos reais em vez de in-process no clone. Isso é lever de **build do busybox**, não
  do backend: exige `FEATURE_SH_STANDALONE` desligado (+ resolução de applet por PATH/
  symlink) **ou** build `!BB_MMU` (vfork+exec self, com `CONFIG_BUSYBOX_EXEC_PATH` no
  caminho absoluto do busybox). Não foi aplicado porque muda o binário do busybox e precisa
  de build+boot no VM para validar. O `execve` já está pronto para receber esse caminho.
- **Confirmação:** rodar qualquer comando forkeado no VM e ler `/fork.log` (tags `C-rdok`/
  `C-rdcnt`) — decide se basta o lever de userland ou se cabe mitigar o read no backend.

## 16. CAUSA RAIZ FINAL e correção (2026-07-17, resolvido e validado no VM)

A instrumentação do `fork` (probe + trace de syscalls no filho clonado) fechou o caso com
números em `/fork.log`. Sequência de eliminação:

1. **`read` de arquivo FUNCIONA no clone** (`C-rdcnt=64`) — a hipótese inicial estava errada.
2. **`read` de pipe herdado FUNCIONA** (`C-iprcnt=2`), **`DuplicateHandle` FUNCIONA**
   (`C-dupok=1`). Só **`CreatePipe` dentro do clone falha** (`C-pipe=6`), mas o pipeline
   cria o pipe no **pai** — então isso não era o problema do `echo | wc`.
3. **O trace de syscalls revelou tudo**: o filho do `cat` faz
   `set_tid_address → sigsetup → execve` e o `X-arg0=/proc/self/exe`. **O `cat` não roda
   in-process — o BusyBox faz `fork` + `execve("/proc/self/exe")`** (re-exec de si mesmo,
   como todo applet **não-NOEXEC**). O `ls` (`APPLET_NOEXEC`) roda in-process e por isso
   sempre funcionou; o `echo` é builtin.
4. **Marcadores no `execve` localizaram o crash**: `X-arg0 → X-res → X-alloc → X-bld →
   X-precp`, e **nada depois** — ou seja, o **`CreateProcessW` CRASHA dentro do clone**
   (o CSR/loader do NT fica parcial no processo clonado). Sem `X-cpok`/`X-cperr`.

**Causa raiz:** o BusyBox roda applets não-NOEXEC via `fork`+`execve("/proc/self/exe")`.
Na NTUnix o `fork` é `RtlCloneUserProcess` (clone parcial) e **`CreateProcessW` não
sobrevive ao clone** → o applet saía **mudo**. Nunca foi `read`, pipe, musl ou o backend —
todos funcionam no clone. Era **`fork`+`exec` (CreateProcessW no clone)** que é impossível.

**Correção (aplicada e validada):**
- **BusyBox roda TODO applet in-process** (patch no `ash` `tryexec`, aplicado por
  `tools/configure-busybox`), como já fazia com os NOEXEC — evita o `exec` que o clone não
  aguenta. `cat`, `wc`, `sort`, pipelines: todos funcionam.
- Backend: **`/proc/self/exe` resolve** para o binário real (`GetModuleFileNameW`,
  `ntpath.c`); **`execve` com `STARTF_USESTDHANDLES`** (para o dia que exec rodar fora do
  clone, ex.: `posix_spawn`); **devfs** (`/dev/null` etc.); **pipe `O_NONBLOCK`** na leitura;
  **propagação de `devkind`** em dup/dup2.

Validação: `cat /etc/passwd` imprime o conteúdo; `echo 1 2 3 | wc -l`→`1`;
`ls / | wc -l`→`7`; suíte busybox no Wine: PASS.

**`posix_spawn` NT-nativo (implementado):** para rodar **binários externos reais** (apps
Unix buildados-para-NTUnix E apps Windows — ambos são PE), há `posix_spawn` sem clone:
`CreateProcessW` no **processo atual** (nunca um clone), com as file_actions aplicadas nos
fds do próprio processo e restauradas depois. Peças: `nt_sys_spawn` (`sys_proc.c`, pseudo-
syscall `NT_SYS_spawn`), override `override/posix_spawn.c` (pula o da musl), `posix_spawnp`
cai nele (sem PATH-search por ora). Registra o filho pro `wait4`. Testado no Wine
(`test/spawn.c`): spawn+wait+file_actions rodando `hello.exe` (Unix) e `cmd.exe` (Windows).

**Roteamento no ash (feito):** o `ash` agora roda comando externo real por `posix_spawn`
em vez de `fork`+`exec` (patch `patches/busybox-ntunix-ash.patch`, helper `spawn_external`
+ hook no `evalcommand`). No **shell principal** (`rootshell`), um comando não-applet é
resolvido como no `shellexec` (direto se tem `/`,`\` ou `C:`; senão PATH-search), roda via
`posix_spawn` herdando as redireções já aplicadas e o env de `listvars`, e espera síncrono
propagando o exit code. Roda binário Unix-para-NTUnix e app Windows. Validado no Wine:
`ash -c './hello.exe; echo $?'` → saída + rc=0; PATH-search idem.

**Limitação remanescente:** comando externo **dentro de pipeline/subshell/background** roda
num filho já clonado (`rootshell==0`), onde `posix_spawn` (CreateProcessW) também crasharia
— então esses seguem o caminho fork+exec antigo (que falha no clone). Fica para depois:
rotear também os forks de pipeline (`evalpipe`) por `posix_spawn`. Applets nesses contextos
funcionam (rodam in-process no clone).

### Deixado como está (limitação estrutural, documentado, não “bug fora do lugar”)
Entrega de sinal (SIGPIPE/SIGINT/SIGCHLD), grupos de processo/job control, threads
(`pthread_create`), `execve` preservar pid, mmap MAP_FIXED real e granularidade 64K,
munmap parcial devolver espaço, access X_OK por bit real, F_SETLK. São reescritas grandes
ou impossíveis sobre Win32 puro; arriscá-las às cegas (sem rodar no VM) regridiria mais do
que corrige.
