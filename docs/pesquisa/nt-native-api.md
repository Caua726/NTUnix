# NT Native API como substrato da `libntposix`

> Pesquisa técnica para a frente "Native API do NT" do NTUnix.
> Objetivo: mapear as funções `Nt*`/`Rtl*` reais que a `libntposix` vai usar
> para traduzir POSIX → NT sem passar pelo Win32 quando possível.
> Escopo alvo: Windows 11 x64 (ntdll/ntoskrnl atuais).
>
> Convenções deste documento:
> - **Grau de dificuldade/risco** é dado numa escala 1–5 (1 = trivial e estável,
>   5 = pesquisa de ponta / semântica hostil ao NT).
> - "v0" = primeira `libntposix` mínima, hospedada dentro do Windows (Fase 1 da VISAO §25),
>   PE 64-bit, single-process, o suficiente para rodar um shell + coreutils simples.

---

## 0. Resumo da estratégia

O NT tem, na prática, **três camadas** que a `libntposix` pode chamar:

```
POSIX (open/read/mmap/pthread…)
   │
   ├─►  Win32   (kernel32/KernelBase: CreateFileW, VirtualAlloc, CreateThread…)   ← atalho pragmático
   │
   └─►  Native API  (ntdll: NtCreateFile, NtAllocateVirtualMemory, NtCreateThreadEx…)  ← alvo final
             │
             └─►  syscall boundary  (int 2Eh / syscall) → ntoskrnl (System Service Dispatch)
```

**Decisão de arquitetura recomendada para a v0:** a `libntposix` deve falar com o **ntdll**
(camada Native API, funções exportadas `Nt*`/`Rtl*`), **nunca** emitir `syscall` própria com número
de serviço fixo. O boundary de syscall (os *service numbers*) é **instável entre builds** e é o único
contrato que a Microsoft explicitamente não garante; o contrato estável são os **stubs exportados no
ntdll** ([j00ru, Windows System Call Tables](https://j00ru.vexillium.org/syscalls/nt/64/);
[HN: syscall numbers change every release](https://news.ycombinator.com/item?id=20855162)).

Onde a Native API for cara/arriscada demais para a v0 (ex.: processos, sockets), é **legítimo e
recomendado** cair para o Win32 primeiro e migrar para `Nt*` depois — isso está alinhado com a VISAO
§26 ("Tudo inicialmente poderia rodar sobre Win32, migrando gradualmente para Native API").

---

## 1. Arquitetura ntdll / Native API

### 1.1 O que é a Native API

A Native API é o conjunto de funções exportadas pelo **`ntdll.dll`** que formam a fronteira
documentada-de-fato entre user mode e o NT Executive. Ela tem duas naturezas bem distintas:

- **`Nt*` / `Zw*`** — *system service stubs*. Cada `NtXxx` é um stub minúsculo que carrega o número
  do serviço no registrador `eax`/`rax` e executa a instrução `syscall` (x64) / `sysenter`/`int 2Eh`
  (x86 legado), transferindo para o **System Service Dispatcher (KiSystemService/KiSystemCall64)** no
  ntoskrnl. A partir do user mode, `NtReadFile` e `ZwReadFile` **apontam para o mesmo stub**; a
  diferença `Nt` vs `Zw` só importa dentro do kernel (o `Zw` zera o *previous mode* para `KernelMode`,
  desligando checagens de acesso de parâmetros vindos do kernel). Em user mode, **use `Nt*`**.
  (Windows Internals 7th ed., cap. "System Service Dispatching"; [phnt](https://github.com/winsiderss/phnt)).

- **`Rtl*`** — *Run-Time Library*. Rotinas **inteiramente em user mode**, dentro do próprio ntdll, que
  **não** cruzam para o kernel: manipulação de strings (`RtlInitUnicodeString`), heap
  (`RtlAllocateHeap`), conversão de path (`RtlDosPathNameToNtPathName_U`), listas, bitmaps,
  time (`RtlSystemTimeToLocalTime`), criação de parâmetros de processo
  (`RtlCreateProcessParametersEx`), etc. São a "libc do ntdll" e são extremamente úteis para a
  `libntposix` porque implementam lógica chata (parsing de path, heap) que não queremos reescrever.

Além disso, o ntdll hospeda o **loader (`Ldr*`)** — `LdrLoadDll`, `LdrGetProcedureAddress` — que é o
equivalente NT nativo de `dlopen`/`dlsym`, e o **runtime de exceções/APC**. O ntdll é a **primeira**
DLL mapeada em todo processo NT (antes até do kernel32); um "minimal/native process" pode rodar só com
ntdll — é o que fazem `smss.exe`, o autochk e o próprio pico-process do WSL1.

### 1.2 Estabilidade — o que é contrato e o que é volátil

| Interface | Estabilidade | Observação |
|---|---|---|
| **Números de syscall** (índice em `eax`) | **Volátil** (muda a cada build, até entre 10 1809→1903) | NUNCA hardcodar. [j00ru](https://j00ru.vexillium.org/syscalls/nt/64/) |
| **Stubs `Nt*` exportados no ntdll** | **Estável de fato** — assinaturas praticamente imutáveis desde NT 3.1/4.0 | Contrato real. Ligar por nome, resolver via `GetProcAddress`/`LdrGetProcedureAddress` |
| **`Rtl*` documentadas** | Estável | ex.: `RtlDosPathNameToNtPathName_U` é usada por todo o Win32 |
| **Estruturas com `Reserved`/campos internos** (PEB, TEB, `PS_CREATE_INFO`) | Parcialmente volátil | offsets podem crescer; usar layouts do **phnt** e nunca assumir tamanho fixo além do documentado |
| **KUSER_SHARED_DATA @ 0x7FFE0000** | Endereço e offsets dos campos base: **estáveis há décadas** | ver §6 |

Regra prática para o NTUnix: **linkar contra os stubs exportados do ntdll por nome** e obter os
protótipos do projeto **phnt** (winsiderss/phnt), que é a coleção de headers Native API mais atual e é
mantida a partir de headers/símbolos oficiais + engenharia reversa desde 2009
([winsiderss/phnt](https://github.com/winsiderss/phnt)). Documentação por função:
[NtDoc / ntdoc.m417z.com](https://ntdoc.m417z.com) e
[ntinternals.net](http://undocumented.ntinternals.net).

### 1.3 Como linkar

O ntdll **não** vem com uma import library completa no Windows SDK (só um subconjunto em
`ntdll.lib`/`ntdllp.lib` do WDK). Duas opções para a `libntposix`:

1. **Import lib do WDK** (`ntdll.lib`) para os `Nt*` documentados + `GetProcAddress(GetModuleHandle("ntdll.dll"), "Nt…")`
   em runtime para o resto. Simples, recomendado para v0.
2. Gerar `.def`/import lib própria a partir da tabela de exports do ntdll (o que ReactOS/Wine fazem).

**Risco 1.x geral: 1–2.** Bem trilhado. O risco real é disciplina: resolver por nome, nunca por
número, e usar phnt como fonte dos structs.

---

## 2. Arquivos — `open/read/write/close/lseek/stat`

### 2.1 As funções

```c
// ntdll — protótipos (phnt / ntifs.h)
NTSTATUS NtCreateFile(
    PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
    ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions,
    PVOID EaBuffer, ULONG EaLength);

NTSTATUS NtOpenFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PIO_STATUS_BLOCK,
                    ULONG ShareAccess, ULONG OpenOptions);

NTSTATUS NtReadFile (HANDLE, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                     PIO_STATUS_BLOCK, PVOID Buffer, ULONG Length,
                     PLARGE_INTEGER ByteOffset, PULONG Key);
NTSTATUS NtWriteFile(HANDLE, HANDLE, PIO_APC_ROUTINE, PVOID, PIO_STATUS_BLOCK,
                     PVOID Buffer, ULONG Length, PLARGE_INTEGER ByteOffset, PULONG Key);
NTSTATUS NtClose(HANDLE);
NTSTATUS NtQueryInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
NTSTATUS NtSetInformationFile(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
```
Refs: [NtCreateFile (winternl)](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile),
[NtCreateFile (ntifs)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntcreatefile),
[NtReadFile devnotes](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntreadfile),
[NtDoc/NtCreateFile](https://ntdoc.m417z.com/ntcreatefile).

### 2.2 As estruturas de apoio

```c
typedef struct _UNICODE_STRING { USHORT Length; USHORT MaximumLength; PWSTR Buffer; } UNICODE_STRING;
// Length/MaximumLength são em BYTES, não em caracteres. Buffer é UTF-16, não precisa ser NUL-terminada.

typedef struct _OBJECT_ATTRIBUTES {
    ULONG Length;                 // = sizeof(OBJECT_ATTRIBUTES)
    HANDLE RootDirectory;         // NULL, ou handle de dir para path relativo (base p/ *at())
    PUNICODE_STRING ObjectName;   // o path NT
    ULONG Attributes;             // OBJ_CASE_INSENSITIVE, OBJ_INHERIT, OBJ_KERNEL_HANDLE…
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} OBJECT_ATTRIBUTES;
// Inicializar com a macro InitializeObjectAttributes(...).

typedef struct _IO_STATUS_BLOCK {
    union { NTSTATUS Status; PVOID Pointer; };
    ULONG_PTR Information;        // p/ read/write: nº de bytes transferidos; p/ create: FILE_CREATED/FILE_OPENED…
} IO_STATUS_BLOCK;
```

### 2.3 O namespace de objetos e os prefixos de path

O Object Manager do NT tem uma árvore de nomes global. Os pontos que importam:

- **`\??\`** (antigamente `\DosDevices\`) — o diretório de *símbolos DOS por sessão*. É onde vivem
  `C:`, `D:`, `\??\C:` → symlink para `\Device\HarddiskVolume3`. **É o prefixo que a `libntposix`
  usará para a maioria dos arquivos.**
- **`\Device\...`** — os device objects reais (`\Device\HarddiskVolume3`, `\Device\Afd`,
  `\Device\NamedPipe`, `\Device\Null`, `\Device\ConDrv` p/ console).
- **`\??\C:\NTUnix\etc\network.conf`** é exatamente o formato que a VISAO §12 já previu.
- Prefixo Win32 `\\?\` (que o CreateFile aceita) **não** é NT-nativo: o ntdll o converte para `\??\`.
- Detalhe importante: NT é **case-sensitive por padrão**; passar `OBJ_CASE_INSENSITIVE` para
  aproximar o comportamento Win32. Para POSIX real (case-sensitive) isso vira uma política por-mount.

Referência canônica de todas as regras: [Google Project Zero — "The Definitive Guide on Win32 to NT
Path Conversion"](https://projectzero.google/2016/02/the-definitive-guide-on-win32-to-nt.html).

### 2.4 Traduzir `/etc/x` → path NT

Dois caminhos:

**(a) Fácil / v0 — reusar o Rtl:** compor o path Win32 (como o `ntupath.c` atual já faz:
`/etc/x` → `C:\NTUnix\etc\x`) e chamar
`RtlDosPathNameToNtPathName_U_WithStatus(win32path, &nt_ustr, NULL, NULL)`. Ele resolve unidade,
`.`/`..`, adiciona o prefixo `\??\` e devolve uma `UNICODE_STRING` pronta para o `OBJECT_ATTRIBUTES`.
Versão `_WithStatus` retorna `NTSTATUS` (a antiga retorna `BOOLEAN`).
Refs: [RtlDosPathNameToNtPathName_U_WithStatus](https://learn.microsoft.com/en-us/windows/win32/devnotes/rtldospathnametontpathname_u_withstatus),
[Mecanik: Convert DOS and NT paths](https://mecanik.dev/en/posts/convert-dos-and-nt-paths-using-rtl-functions/),
[ReactOS apitest](https://doxygen.reactos.org/dc/dfc/RtlDosPathNameToNtPathName__U_8c_source.html).

**(b) Definitivo — resolução própria:** a `libntposix` mantém sua própria noção de raiz
(`NTUNIX_ROOT`), CWD virtual e tabela de mounts (`/`, `/proc`, `/dev`…), e monta a `UNICODE_STRING`
`\??\C:\NTUnix\...` diretamente com `RtlInitUnicodeString` + concat, sem depender do CWD do processo
Win32. Isso é necessário para semântica POSIX correta (symlinks, `/proc`, chroot, case-sensitivity) e
para não vazar a noção de "letra de unidade" (VISAO §12 diz que apps NTUnix não conhecem letras).

**`openat`/`*at()`:** o campo `RootDirectory` do `OBJECT_ATTRIBUTES` recebe o HANDLE de um diretório
aberto e o `ObjectName` vira **relativo** a ele — mapeia quase 1:1 para os `*at()` do POSIX (`openat`,
`unlinkat`), o que é uma vantagem grande do NT.

### 2.5 Semântica de leitura/escrita

- Para arquivos comuns, abra com `CreateOptions |= FILE_SYNCHRONOUS_IO_NONALERT`. Isso faz o I/O
  Manager **manter o ponteiro de arquivo** e serializar as operações — assim `NtReadFile` com
  `ByteOffset = NULL` (ou `LowPart = FILE_USE_FILE_POINTER_POSITION`, `HighPart = -1`) lê a partir da
  posição corrente, exatamente como o `read()` POSIX. O `Information` do `IO_STATUS_BLOCK` traz os
  bytes lidos. ([NtReadFile devnotes](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntreadfile)).
- `lseek` → `NtSetInformationFile` com `FilePositionInformation` (ou manter a posição na própria
  struct do fd da libntposix, o que é mais barato e é o que Cygwin faz).
- `fstat`/`stat` → `NtQueryInformationFile(FileAllInformation / FileBasicInformation /
  FileStandardInformation)` e `NtQueryAttributesFile`. Mapear `FILE_BASIC_INFORMATION` (timestamps em
  100ns desde 1601) para `struct stat` (segundos desde 1970) — a conversão de epoch é trivial.
- `unlink` → abrir com `DELETE` e `NtSetInformationFile(FileDispositionInformation, DeleteFile=TRUE)`
  (ou `FileDispositionInformationEx` com `POSIX_SEMANTICS` no Win10+, que permite deletar arquivo ainda
  aberto — importante para semântica POSIX de "unlink de arquivo aberto").
- Diretórios → `NtQueryDirectoryFile`/`NtQueryDirectoryFileEx` para `readdir`.

### 2.6 Grau e recomendação

**Dificuldade: 2 (arquivos comuns).** É a área mais madura e a de maior payoff. As assinaturas são
estáveis, o mapeamento é direto, e ReactOS/Wine têm implementações de referência dos apitests.

**Atacar primeiro na v0:** `open/openat/read/write/close/lseek/stat/fstat/unlink/mkdir/getdents`.
Isso já roda um `cat`, `ls`, `cp`. Recomendo começar com a rota **(a)** (Rtl) para path e evoluir para
**(b)** (resolução própria + mounts) quando `/proc` e `/dev` entrarem.

---

## 3. Memória — `mmap`, `munmap`, `brk`, `malloc`

### 3.1 As funções

```c
// Anônima / brk-like:
NTSTATUS NtAllocateVirtualMemory(HANDLE Process, PVOID *BaseAddress, ULONG_PTR ZeroBits,
                                 PSIZE_T RegionSize, ULONG AllocationType, ULONG Protect);
NTSTATUS NtFreeVirtualMemory(HANDLE, PVOID *BaseAddress, PSIZE_T RegionSize, ULONG FreeType);
NTSTATUS NtProtectVirtualMemory(HANDLE, PVOID *, PSIZE_T, ULONG NewProtect, PULONG OldProtect);

// Mapeamento de arquivo / shared memory:
NTSTATUS NtCreateSection(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, PLARGE_INTEGER MaximumSize,
                         ULONG SectionPageProtection, ULONG AllocationAttributes, HANDLE FileHandle);
NTSTATUS NtMapViewOfSection(HANDLE Section, HANDLE Process, PVOID *BaseAddress, ULONG_PTR ZeroBits,
                            SIZE_T CommitSize, PLARGE_INTEGER SectionOffset, PSIZE_T ViewSize,
                            SECTION_INHERIT InheritDisposition, ULONG AllocationType, ULONG Win32Protect);
NTSTATUS NtUnmapViewOfSection(HANDLE Process, PVOID BaseAddress);
NTSTATUS NtFlushVirtualMemory(HANDLE, PVOID *, PSIZE_T, PIO_STATUS_BLOCK);   // msync
```
Refs: [NtMapViewOfSection](https://ntdoc.m417z.com/ntmapviewofsection),
[NtCreateSection](https://ntdoc.m417z.com/ntcreatesection),
[ReactOS NtMapViewOfSection apitest](https://doxygen.reactos.org/da/d9d/NtMapViewOfSection_8c_source.html).

### 3.2 Mapeamentos POSIX → NT

| POSIX | NT | Notas |
|---|---|---|
| `mmap(MAP_ANONYMOUS)` | `NtAllocateVirtualMemory(MEM_RESERVE\|MEM_COMMIT, PAGE_READWRITE)` | direto; `MAP_NORESERVE` ≈ só `MEM_RESERVE` |
| `mmap(fd, MAP_SHARED)` | `NtCreateSection(fileHandle, SEC_COMMIT)` + `NtMapViewOfSection(ViewShare)` | escrita volta pro arquivo |
| `mmap(fd, MAP_PRIVATE)` | `NtCreateSection` + `NtMapViewOfSection(ViewUnmap, PAGE_WRITECOPY)` | copy-on-write |
| `mmap(MAP_FIXED)` | passar `*BaseAddress != NULL` | ver §3.3 (limitação séria) |
| `munmap` (região inteira) | `NtUnmapViewOfSection` (se view) / `NtFreeVirtualMemory(MEM_RELEASE)` | ver §3.3 |
| `mprotect` | `NtProtectVirtualMemory` | granularidade de página |
| `msync` | `NtFlushVirtualMemory` | |
| `brk`/`sbrk` | reservar arena grande com `NtAllocateVirtualMemory(MEM_RESERVE)` e commitar em pedaços | musl/newlib normalmente nem usam `brk`, usam `mmap` |

- `NtCreateSection` sem `FileHandle` (`= NULL`) cria seção **paginada (pagefile-backed)** — é o
  equivalente de `MAP_ANONYMOUS | MAP_SHARED`, útil para memória compartilhada entre processos e para
  o buffer de `shm_open`.
- Flags: `SEC_COMMIT` (commit imediato), `SEC_RESERVE`, `SEC_IMAGE` (mapear PE como imagem —
  **é assim que se carrega executável/`.so`-como-DLL**), `SEC_LARGE_PAGES`.
- `MaximumSize` é arredondado p/ página; se pagefile-backed, define o tamanho real da seção.

### 3.3 As armadilhas (o NT não é Unix aqui)

1. **Granularidade de alocação = 64 KB.** Views de seção e reservas de VM são alinhados em 64 KB
   (`dwAllocationGranularity`), não em 4 KB. `mmap` POSIX promete alinhamento de página (4 KB). Para
   `mmap` de arquivo em offsets não-múltiplos de 64 KB, a `libntposix` precisa mapear a partir do
   múltiplo de 64 KB abaixo e devolver o ponteiro ajustado — exatamente o que o Wine/MSYS fazem.
   (Já anotado no memory index: "buffers grandes via mmap_anon (bins capa 64KB)").
2. **Não dá para desmapear *parte* de uma view.** `NtUnmapViewOfSection` desfaz a **view inteira**;
   `munmap` parcial (dividir uma região) não tem equivalente direto. Solução: mapear cada `mmap` como
   sua própria view e manter na `libntposix` uma tabela de intervalos, ou usar
   `NtMapViewOfSection` com placeholders (`MEM_REPLACE_PLACEHOLDER`, Win10+) para simular split.
3. **`MAP_FIXED` sobre região já mapeada** é problemático: o NT não deixa mapear por cima
   atomicamente sem placeholder. Placeholders (`VirtualAlloc2`/`MapViewOfFile3` no Win32, ou o
   equivalente Nt com `MEM_RESERVE_PLACEHOLDER`) resolvem, mas são Win10+ e mais complexos.
4. `mmap` anônima grande + `fork` = o problema de reconstruir os mapeamentos no filho (ver §5).

### 3.4 Grau e recomendação

**Dificuldade: 2 (anon/brk) — 3 (mmap de arquivo com offsets/`MAP_FIXED`).**

**Atacar primeiro na v0:** `NtAllocateVirtualMemory`/`NtFreeVirtualMemory` para o alocador anônimo —
isso já satisfaz o `malloc` da musl/newlib (que preferem `mmap` anônima a `brk`). Fazer o `mmap` de
arquivo `MAP_PRIVATE`/`MAP_SHARED` sem `MAP_FIXED` na sequência (via section). Deixar `MAP_FIXED`
sobre região existente e `munmap` parcial para depois, com a estratégia de placeholder.

---

## 4. Threads e sincronização — `pthread`, `futex`

### 4.1 Criação de thread

```c
NTSTATUS NtCreateThreadEx(
    PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
    HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument,
    ULONG CreateFlags,               // THREAD_CREATE_FLAGS_CREATE_SUSPENDED etc.
    SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize,
    PPS_ATTRIBUTE_LIST AttributeList);
NTSTATUS NtTerminateThread(HANDLE, NTSTATUS ExitStatus);
```
Ref: [NtCreateThreadEx / NtDoc](https://ntdoc.m417z.com/ntcreatethreadex),
[phnt/ntpsapi.h](https://github.com/winsiderss/phnt/blob/master/ntpsapi.h).

`NtCreateThreadEx` (Vista+) é o caminho nativo que o próprio `CreateThread`/`CreateRemoteThreadEx`
usa por baixo. Ele **já cuida da criação da stack, do TEB e da inicialização do TLS do loader** — ao
contrário do antigo `NtCreateThread`, que exigia montar o `CONTEXT` e o `INITIAL_TEB` na mão. Para a
`libntposix`, `pthread_create` → `NtCreateThreadEx`, e o TLS de POSIX pode montar sobre o TEB
(`TlsAlloc`/slots do TEB, ou o próprio TLS do PE).

**Cuidado:** uma thread nativa precisa que o **loader lock** e as estruturas de TLS estejam prontas.
Se a `libntposix` roda dentro de um processo Win32 normal (v0), isso já está resolvido pelo ntdll. Em
processo nativo puro (sem kernel32), há trabalho extra de inicialização.

### 4.2 Sincronização — a peça-chave: WaitOnAddress / NtWaitForAlertByThreadId

O NT moderno tem **duas gerações** de primitiva "futex-like", ambas úteis:

**(a) Keyed Events — `NtWaitForKeyedEvent` / `NtReleaseKeyedEvent`** (XP+).
Um "keyed event" é um objeto onde thread espera/sinaliza usando um **ponteiro (chave)** como
identificador. Foi introduzido para permitir cadeados sem alocar um objeto de evento por lock (resolve
o caso de falta de memória em `SRWLOCK`/`CRITICAL_SECTION`). Existe um keyed event **global implícito**
(`\KernelObjects\CritSecOutOfMemoryEvent`) que qualquer processo pode usar passando `NULL` como handle.
É um futex de fato: chave = endereço, espera/acorda por endereço.
([Medium/Windows Internals: Thread Sync Primitives](https://medium.com/windows-os-internals/windows-internals-thread-synchronization-primitives-0b222b71f0ce)).

**(b) `WaitOnAddress` / `WakeByAddressSingle` / `WakeByAddressAll`** (Win8+) — a API futex "de verdade",
e a que a `libntposix` deve mirar. Semântica quase idêntica ao `futex(FUTEX_WAIT/WAKE)` do Linux:
espera enquanto `*addr == valorEsperado`, acorda por endereço. Suporta tamanhos 1/2/4/8 bytes.
Por baixo, no ntdll ela é `RtlWaitOnAddress`/`RtlWakeAddress*`, e nos builds atuais é implementada
sobre os *thread-alert* syscalls **`NtWaitForAlertByThreadId` / `NtAlertThreadByThreadIdEx`** (park/unpark
por thread-id), com uma tabela de waiters no processo — **não** aloca objeto de kernel por lock, é
process-local. Isso é confirmado pela implementação do Wine e por análises de internals.
Refs: [WaitOnAddress (synchapi)](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress),
[wine/dlls/ntdll/sync.c](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/sync.c),
[shift.click: Futex-likes](https://shift.click/blog/futex-like-apis/).

**Consequência para pthreads:** com `WaitOnAddress`/`WakeByAddress*` (ou keyed events como fallback
pré-Win8), dá para implementar **do zero e process-local**:
- `pthread_mutex_t` → estado atômico + `WaitOnAddress` no slow path (design idêntico ao mutex de
  futex do musl/glibc). `futex()` do POSIX/Linux emulado → `WaitOnAddress`/`WakeBy*` (VISAO §6 já
  aponta isso).
- `pthread_cond_t` → contador de sequência + `WaitOnAddress` (padrão idêntico ao `CONDITION_VARIABLE`
  do Win32, que hoje é implementado exatamente assim).
- `sem_t`, `pthread_rwlock_t`, `pthread_once` → todos derivam da mesma primitiva.
- barreiras/`pthread_barrier` idem.

Alternativa "atalho": usar diretamente `SRWLOCK` + `CONDITION_VARIABLE` do KernelBase (que já são
futex por baixo). Mais rápido de escrever, mas amarra a `libntposix` ao Win32; para a v0 é aceitável,
para o alvo final prefira reimplementar sobre `Rtl*`/`Nt*`.

Para timeouts (`pthread_mutex_timedlock`, `sem_timedwait`) o `WaitOnAddress` aceita timeout em
milissegundos; a conversão de `struct timespec` é trivial.

### 4.3 Grau e recomendação

**Dificuldade: 2 (threads) — 3 (a biblioteca pthread completa e correta).**

**Atacar primeiro na v0:** `NtCreateThreadEx` + `pthread_mutex`/`pthread_cond`/`pthread_once`/`sem`
sobre `WaitOnAddress`/`WakeByAddress*`. Essa é uma das áreas de **melhor razão esforço/retorno** do NT:
a primitiva futex existe e é boa. Um `futex()` emulado sobre `WaitOnAddress` cobre a maior parte do que
software Linux portado espera. Manter keyed events só como nota histórica/fallback.

---

## 5. Processos — `fork` e `exec` (a área mais dura)

### 5.1 Por que `fork()` é difícil no NT

O NT **não tem** primitiva de "duplicar o processo atual com toda a sua VM em COW e continuar os dois
do mesmo ponto". O modelo NT é o oposto do Unix: cria-se um processo **vazio** e monta-se o endereço a
partir de uma imagem. Os obstáculos concretos:

1. Um processo Win32 útil precisa estar **conectado ao CSRSS** (o subsistema Win32) e ter PEB/heaps/
   TLS/loader inicializados. Um processo criado "cru" via `NtCreateProcess` que compartilhe a VM do pai
   em COW **não está conectado ao csrss** e é, nas palavras dos devs do Cygwin, "inútil" para código
   Win32. ([cygwin-developers: NtCreateProcess redux](https://cygwin.com/pipermail/cygwin-developers/2011-April/010277.html)).
2. Reconstruir no filho o estado que não é COW-friendly: handles, mmaps, TLS, loader.

### 5.2 As três estratégias reais (e o que o NTUnix deve fazer)

**Estratégia A — "Cygwin fork": copy + fixup (sem COW).**
O Cygwin faz `fork` assim ([Cygwin User's Guide, "Highlights"](https://cygwin.com/cygwin-ug-net/highlights.html)):
1. Pai cria filho **suspenso** com `CreateProcess` (mesmo exe).
2. Pai faz `setjmp` do próprio contexto e guarda em memória compartilhada.
3. Pai **copia `.data`/`.bss`, heap e stack** do próprio espaço para o espaço do filho suspenso
   (via `NtWriteVirtualMemory`).
4. Pai solta o mutex; o filho recria os mmaps e faz `longjmp`, "retornando" de `fork`.

É **não-COW, lento e complexo**, e é a fonte crônica de fragilidade do Cygwin (ASLR/`\dev` de DLL em
endereços diferentes quebram o layout; por isso o Cygwin usa `rebase` e desliga ASLR na sua DLL). Mas
**funciona sem código-fonte do NT** e mantém o processo conectado ao Win32. É a rota realista para
NTUnix se `fork()` genérico (fork-sem-exec) precisar existir.

**Estratégia B — WSL1 "pico process".**
O WSL1 resolve `fork` de verdade porque tem um **driver de kernel (`lxcore.sys`)** e cria **pico
processes** (processos mínimos, sem DLL do Windows, cujas syscalls são desviadas para o driver). O
`fork` do Linux é atendido pelo `lxcore.sys`, que usa **APIs internas do kernel NT** para criar o
processo com a semântica correta e copiar o resto do estado.
Refs: [MS: Pico Process Overview](https://learn.microsoft.com/en-us/archive/blogs/wsl/pico-process-overview),
[MS: WSL System Calls](https://learn.microsoft.com/en-us/archive/blogs/wsl/wsl-system-calls),
[TrainSec: how WSL1 works](https://trainsec.net/library/windows-internals/windows-subsystem-for-linux/).
**Isso exige um driver kernel-mode e APIs não exportadas** → fora do escopo de uma `libntposix`
puramente user-mode e da v0. É o "endgame" caso o NTUnix decida ter seu próprio subsistema (o que a
VISAO §3 mantém em aberto, já que preserva ntoskrnl/Executive/Process Manager).

**Estratégia C — evitar `fork` puro; casar `fork`+`exec` em `posix_spawn`/`NtCreateUserProcess`.**
A esmagadora maioria dos usos de `fork` em software Unix é `fork()` seguido imediatamente de `exec()`
(shells, `make`, `xargs`). Esse padrão **mapeia bem** para criar um processo novo já com a imagem
final — que é o que o NT faz nativamente e rápido. Recomendação: implementar `posix_spawn` +
`vfork`-que-só-serve-para-exec como **caminho rápido nativo**, e oferecer `fork()` genérico só via
Estratégia A (lento) para os casos que realmente bifurcam sem exec.

### 5.3 `exec` — `NtCreateUserProcess`

```c
NTSTATUS NtCreateUserProcess(
    PHANDLE ProcessHandle, PHANDLE ThreadHandle,
    ACCESS_MASK ProcessDesiredAccess, ACCESS_MASK ThreadDesiredAccess,
    POBJECT_ATTRIBUTES ProcessObjectAttributes, POBJECT_ATTRIBUTES ThreadObjectAttributes,
    ULONG ProcessFlags, ULONG ThreadFlags,
    PRTL_USER_PROCESS_PARAMETERS ProcessParameters,
    PPS_CREATE_INFO CreateInfo, PPS_ATTRIBUTE_LIST AttributeList);
```
`NtCreateUserProcess` (Vista+) cria processo **e** thread inicial numa chamada, e é o que
`CreateProcessW` usa por baixo. Fluxo:
1. `RtlCreateProcessParametersEx(&params, &imagePath, dllPath, cwd, cmdLine, env, windowTitle, …)`
   monta o `RTL_USER_PROCESS_PARAMETERS` (linha de comando, env block, cwd, std handles).
2. Preencher `PS_CREATE_INFO` (`Size`, `State = PsCreateInitialState`) e um `PS_ATTRIBUTE_LIST` com
   pelo menos `PS_ATTRIBUTE_IMAGE_NAME` (path NT do exe); opcionalmente `PS_ATTRIBUTE_PARENT_PROCESS`
   (PPID spoof / reparent), `PS_ATTRIBUTE_STD_HANDLE_INFO`, token, etc.
3. Chamar `NtCreateUserProcess`.
Refs: [NtCreateUserProcess / NtDoc](https://ntdoc.m417z.com/ntcreateuserprocess),
[capt-meelo: Making NtCreateUserProcess Work](https://captmeelo.com/redteam/maldev/2022/05/10/ntcreateuserprocess.html),
[offensivedefence: NtCreateUserProcess](https://offensivedefence.co.uk/posts/nt-create-user-process/).

A herança de fds POSIX no `exec` se faz pelos **std handles** e pela lista de handles herdáveis (ver §7).

**Nota sobre PID:** o NT tem *Client ID* (PID/TID) mas a semântica de *process groups*, *sessions*,
`waitpid`, `SIGCHLD` é POSIX e **não** existe no NT. A `libntposix` precisa de uma **tabela de PIDs
virtuais** própria e usar `NtWaitForSingleObject` no handle do processo + `NtQueryInformationProcess`
(`ProcessBasicInformation` → `ExitStatus`) para implementar `waitpid`. Sinais (`kill`, `SIGTERM`,
handlers) não têm equivalente NT e são um subsistema à parte (tipicamente implementado com um
thread/named-pipe/evento por processo, como faz o Cygwin) — **grande e fora da v0**.

### 5.4 Grau e recomendação

**Dificuldade: `exec`/`posix_spawn` = 3. `fork`+exec casado = 3. `fork()` genérico = 5. Sinais = 4–5.**

**Atacar primeiro na v0:** `posix_spawn`/`spawn` nativo via `NtCreateUserProcess` +
`RtlCreateProcessParametersEx`, e `waitpid` via handle do processo. **Não** tentar `fork()` COW real
na v0. Se o shell escolhido precisar de `fork`, ou (i) usar um shell que aceite `posix_spawn`, ou (ii)
implementar a Estratégia A (Cygwin-style) como item isolado e assumidamente lento. Sinais: começar só
com o mínimo (`SIGKILL`/`SIGTERM` → `NtTerminateProcess`; `SIGCHLD` sintetizado no `waitpid`).

Esta é **a área que define o teto de compatibilidade** do NTUnix e a que a VISAO §23 corretamente
lista como principal obstáculo. Recomendo tratá-la como projeto próprio depois que arquivos/mem/threads
estiverem verdes.

---

## 6. Tempo e I/O assíncrono

### 6.1 Relógios

**Caminho ultra-rápido (sem syscall): `KUSER_SHARED_DATA` em `0x7FFE0000`.** Essa página é mapeada
read-only em **todo** processo, em endereço fixo (32 e 64 bits), e contém campos atualizados pelo
kernel:
- `SystemTime` (100 ns desde 1601-01-01, UTC) → `clock_gettime(CLOCK_REALTIME)`.
- `InterruptTime` (100 ns desde boot, **monotônico**) → `clock_gettime(CLOCK_MONOTONIC)`.
- `TickCountQuad`, `BaselineSystemTimeQpc`/`BaselineInterruptTimeQpc` (para compor tempo via QPC).
Ler direto dessa página evita syscall — é o que `GetSystemTimeAsFileTime`/`QueryInterruptTime` fazem.
Endereço e offsets base são estáveis há décadas.
Refs: [geoffchappell: KUSER_SHARED_DATA](https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/api/ntexapi_x/kuser_shared_data/index.htm),
[MS Learn: KUSER_SHARED_DATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-kuser_shared_data),
[NtDoc: KUSER_SHARED_DATA](https://ntdoc.m417z.com/kuser_shared_data).

**Caminho via syscall (quando precisar de precisão/serviço):**
- `NtQuerySystemTime(&li)` → realtime.
- `NtQueryPerformanceCounter(&counter, &freq)` → `CLOCK_MONOTONIC` de alta resolução / QPC
  (equivalente a `clock_gettime(CLOCK_MONOTONIC_RAW)`).
- `NtQueryTimerResolution`/`NtSetTimerResolution` para ajustar granularidade.

**Sleep/timers:** `NtDelayExecution(Alertable, &interval)` → `nanosleep`/`usleep`. `NtCreateTimer`/
`NtSetTimer` (ou `NtCreateTimer2`) → `timer_create`/`timerfd`. `interval` é `LARGE_INTEGER` em 100ns
(negativo = relativo, positivo = absoluto).

**Grau: 1–2.** Trivial e estável. **Atacar na v0** (é dependência de quase tudo). `clock_gettime`,
`gettimeofday`, `nanosleep` saem quase de graça.

### 6.2 `epoll`/`poll`/`select` via IOCP e via `\Device\Afd`

Duas necessidades diferentes:

**(a) Multiplexar I/O assíncrono já em curso (arquivos, pipes) → IOCP.**
```c
NTSTATUS NtCreateIoCompletion(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES, ULONG Count);
NTSTATUS NtSetInformationFile(...FileCompletionInformation...);   // associa HANDLE ao port
NTSTATUS NtRemoveIoCompletion  (HANDLE, PVOID *Key, PVOID *ApcCtx, PIO_STATUS_BLOCK, PLARGE_INTEGER Timeout);
NTSTATUS NtRemoveIoCompletionEx(HANDLE, PFILE_IO_COMPLETION_INFORMATION, ULONG Count, PULONG Num, PLARGE_INTEGER, BOOLEAN Alertable);
NTSTATUS NtSetIoCompletion(HANDLE, PVOID Key, PVOID ApcCtx, NTSTATUS, ULONG_PTR Info);  // "eventfd"/wakeup manual
```
Uma I/O Completion Port é uma fila onde o kernel enfileira a conclusão de operações assíncronas.
Associa-se um HANDLE ao port com `NtSetInformationFile(FileCompletionInformation)` e drena-se com
`NtRemoveIoCompletionEx` (batch). `NtSetIoCompletion` permite **postar um pacote manualmente** — é o
truque para acordar o loop (equivalente a escrever num `eventfd`/self-pipe).
Refs: [NtCreateIoCompletion / NtDoc](https://ntdoc.m417z.com/ntcreateiocompletion),
[NtSetIoCompletion / NtDoc](https://ntdoc.m417z.com/ntsetiocompletion),
[Microsoft Press: Understanding the Windows I/O System](https://www.microsoftpressstore.com/articles/article.aspx?p=2201309&seqNum=3).

Diferença semântica que **importa muito**: IOCP é **completion-based** (avisa quando a operação
*terminou*), enquanto `epoll` é **readiness-based** (avisa quando o fd está *pronto* para operar sem
bloquear). Para arquivos/pipes isso se contorna emitindo reads assíncronos, mas para **sockets** o
modelo readiness é o esperado por software portado.

**(b) Readiness de sockets → `\Device\Afd` (o truque do wepoll).**
A forma canônica de obter `epoll`/`poll` *readiness* de sockets no NT é falar direto com o driver de
sockets **`\Device\Afd`** (Auxiliary Function Driver, camada abaixo do Winsock) via
`NtDeviceIoControlFile` com o IOCTL `IOCTL_AFD_POLL` e uma `AFD_POLL_INFO`, entregando as notificações
por uma IOCP. É exatamente assim que a biblioteca **wepoll** implementa `epoll` no Windows, e é o que
**libuv (Node.js), mio/tokio (Rust) e ZeroMQ** usam para async I/O em sockets.
Refs: [piscisaureus/wepoll](https://github.com/piscisaureus/wepoll/blob/dist/wepoll.c),
[Len Holgate: Adventures with \Device\Afd](https://lenholgate.com/blog/2023/04/adventures-with-afd.html),
[notgull: \Device\Afd](https://notgull.net/device-afd/).

**Grau: IOCP = 3; readiness de socket via AFD = 4.** **Recomendação v0:** implementar `poll`/`select`
para poucos fds de forma simples primeiro (loop com `WaitForMultipleObjects`/`NtWaitForMultipleObjects`
sobre eventos, ou AFD_POLL síncrono), e construir o `epoll` "de verdade" (IOCP + AFD) numa fase
posterior, quando entrar rede/servidores. Um `eventfd` sintético sai de graça com `NtSetIoCompletion`
ou um par de handles de evento.

### 6.3 `inotify` e outros

`inotify` → `NtNotifyChangeDirectoryFile`(Ex) (é o que `ReadDirectoryChangesW` usa). Grau 3, fase
posterior — está no roadmap da VISAO §6 mas não é v0.

---

## 7. Handles NT como file descriptors POSIX

### 7.1 O descompasso

| POSIX fd | NT HANDLE |
|---|---|
| inteiro **pequeno**, denso, começa em 0/1/2 | valor **opaco** (múltiplo de 4), esparso |
| `dup`/`dup2` para número **específico** | `NtDuplicateObject` devolve handle **arbitrário** |
| `fd 0/1/2` = stdin/out/err por convenção | `ProcessParameters->StandardInput/Output/Error` |
| herança por `fork`/`exec` conforme `O_CLOEXEC` | flag `OBJ_INHERIT`/`HANDLE_FLAG_INHERIT` por handle |
| `select`/`poll` por número | por HANDLE |
| aponta para arquivo/pipe/socket/tty | HANDLE tem tipo de objeto NT distinto |

Conclusão: **não dá para usar o valor do HANDLE como fd**. A `libntposix` precisa de uma **tabela de
descritores própria** — exatamente o design do Cygwin.

### 7.2 O modelo (copiar do Cygwin `dtable`)

O Cygwin mantém uma `dtable`: um array indexado pelo fd (int pequeno) → ponteiro para um objeto
`fhandler` que encapsula o HANDLE NT + o tipo de dispositivo (disco, pipe, tty, socket, `/dev/null`…)
e sabe fazer o I/O específico daquele tipo. A tabela vive no `cygheap`, uma região que é **copiada
para o filho** no `fork`/`exec`, preservando o mapeamento.
Refs: [Cygwin dtable.cc](https://github.com/msysgit/msys/blob/master/winsup/cygwin/dtable.cc),
[DeepWiki: File Descriptor and I/O System](https://deepwiki.com/msys2/msys2-runtime/2.2-file-descriptor-and-io-system).

Design recomendado para a `libntposix`:
```c
struct fd_entry {
    enum { FD_FILE, FD_PIPE, FD_TTY, FD_SOCKET, FD_DEV, FD_EPOLL, ... } type;
    HANDLE   h;             // handle NT subjacente
    uint64_t pos;           // posição de arquivo (mantida em user mode)
    int      flags;         // O_APPEND, O_NONBLOCK, FD_CLOEXEC...
    // vtable de ops por tipo (read/write/close/ioctl) — estilo fhandler
};
struct fd_table { struct fd_entry *slots; int cap; /* lock */ };  // por processo
```
- **`open`** aloca o menor fd livre → cria HANDLE via `NtCreateFile` → guarda no slot.
- **`dup`/`dup2`** → `NtDuplicateObject(cur, cur, &newh, ...)` e coloca no slot destino (fechando o
  antigo em `dup2`). O número do fd é escolhido pela tabela, não pelo NT.
- **`close`** → `NtClose(h)` + libera o slot.
- **`fcntl(F_SETFD, FD_CLOEXEC)`** → alterna `OBJ_INHERIT`/`HANDLE_FLAG_INHERIT` no HANDLE
  (`NtSetInformationObject` / `SetHandleInformation`) e a flag no slot.
- **stdin/stdout/stderr (0/1/2)** → inicializados a partir de
  `NtCurrentPeb()->ProcessParameters->StandardInput/Output/Error`.
- **herança no `exec`** → os handles marcados herdáveis passam ao filho; a `libntposix` serializa a
  própria `fd_table` (quais slots, tipos, flags, posições) num bloco passado ao filho — via
  `RTL_USER_PROCESS_PARAMETERS` (env var/`Reserved`), memória compartilhada, ou uma seção herdada.
  (O Cygwin passa isso pelo `cygheap` herdado.)

### 7.3 Grau e recomendação

**Dificuldade: 2 (tabela + file/pipe/tty) — 3 (herança correta no spawn e sockets).**

**Atacar primeiro na v0 — é o alicerce.** A `fd_table` deve ser **a primeira coisa** implementada,
antes mesmo de `open`, porque tudo (arquivos, pipes, tty, sockets, epoll) pendura nela. Começar com
`FD_FILE` e os três std handles; adicionar `FD_PIPE` (para `pipe()`/shell) e `FD_TTY` (console via
`\Device\ConDrv`) logo em seguida.

---

## 8. Ordem de ataque recomendada para a `libntposix` v0

Priorização por **(dependência × payoff × baixo risco)**:

1. **`fd_table`** (§7) — alicerce de tudo. Risco 2.
2. **Path translation** (§2.4) — `RtlDosPathNameToNtPathName_U` para começar; resolução própria depois. Risco 2.
3. **Arquivos** (§2) — `open/read/write/close/lseek/stat/getdents/mkdir/unlink`. Risco 2. → já roda `cat`,`ls`,`cp`.
4. **Tempo** (§6.1) — `clock_gettime`/`gettimeofday`/`nanosleep` via KUSER_SHARED_DATA + `NtDelayExecution`. Risco 1.
5. **Memória** (§3) — alocador anônimo via `NtAllocateVirtualMemory` (satisfaz malloc da musl/newlib); depois `mmap` de arquivo. Risco 2–3.
6. **Threads + sync** (§4) — `NtCreateThreadEx` + pthread mutex/cond/once/sem + `futex` sobre `WaitOnAddress`. Risco 2–3.
7. **Pipes + TTY** (§7/§2) — `pipe()` (named pipe/`NtCreatePipe`), console via `\Device\ConDrv`. Risco 2–3.
8. **Spawn/exec** (§5.3) — `posix_spawn`/`NtCreateUserProcess` + `waitpid` via handle. Risco 3. (evitar `fork` COW).
9. *(pós-v0)* sinais, `fork()` genérico estilo-Cygwin, `epoll` real (IOCP+AFD), sockets, `inotify`.

Regra transversal: **sempre linkar `ntdll` por nome**, tipos via **phnt**, e **cair para Win32 sem
culpa** onde a Native API for cara demais para a v0 (VISAO §26) — migrando para `Nt*` incrementalmente.

---

## 9. Fontes principais

**Documentação primária / Microsoft**
- [NtCreateFile (winternl)](https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile) · [NtCreateFile (ntifs)](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntcreatefile) · [NtReadFile devnotes](https://learn.microsoft.com/en-us/windows/win32/devnotes/ntreadfile)
- [WaitOnAddress](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-waitonaddress) · [KUSER_SHARED_DATA](https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/ns-ntddk-kuser_shared_data) · [RtlDosPathNameToNtPathName_U_WithStatus](https://learn.microsoft.com/en-us/windows/win32/devnotes/rtldospathnametontpathname_u_withstatus)
- WSL/pico: [Pico Process Overview](https://learn.microsoft.com/en-us/archive/blogs/wsl/pico-process-overview) · [WSL System Calls](https://learn.microsoft.com/en-us/archive/blogs/wsl/wsl-system-calls) · [AF_UNIX comes to Windows](https://devblogs.microsoft.com/commandline/af_unix-comes-to-windows/)
- [Understanding the Windows I/O System (Microsoft Press)](https://www.microsoftpressstore.com/articles/article.aspx?p=2201309&seqNum=3)

**Headers / referência Native API**
- [winsiderss/phnt](https://github.com/winsiderss/phnt) (headers) · [phnt/ntpsapi.h](https://github.com/winsiderss/phnt/blob/master/ntpsapi.h)
- [ntdoc.m417z.com](https://ntdoc.m417z.com) (por função) · [undocumented.ntinternals.net](http://undocumented.ntinternals.net)
- [geoffchappell.com: KUSER_SHARED_DATA](https://www.geoffchappell.com/studies/windows/km/ntoskrnl/inc/api/ntexapi_x/kuser_shared_data/index.htm)
- [j00ru: Windows Syscall Tables](https://j00ru.vexillium.org/syscalls/nt/64/) · [j00ru/windows-syscalls](https://github.com/j00ru/windows-syscalls)

**ReactOS / Wine (implementações de referência)**
- [ReactOS NtCreateFile apitest](https://doxygen.reactos.org/dd/dbf/NtCreateFile_8c_source.html) · [ReactOS NtMapViewOfSection apitest](https://doxygen.reactos.org/da/d9d/NtMapViewOfSection_8c_source.html) · [ReactOS RtlDosPathNameToNtPathName_U apitest](https://doxygen.reactos.org/dc/dfc/RtlDosPathNameToNtPathName__U_8c_source.html)
- [wine/dlls/ntdll/sync.c](https://github.com/wine-mirror/wine/blob/master/dlls/ntdll/sync.c)

**Cygwin (o precedente mais próximo do NTUnix)**
- [Cygwin User's Guide — Highlights (fork, fds, signals)](https://cygwin.com/cygwin-ug-net/highlights.html) · [Cygwin dtable.cc](https://github.com/msysgit/msys/blob/master/winsup/cygwin/dtable.cc) · [cygwin-developers: NtCreateProcess redux](https://cygwin.com/pipermail/cygwin-developers/2011-April/010277.html)

**Análises técnicas**
- [Project Zero: Win32→NT Path Conversion](https://projectzero.google/2016/02/the-definitive-guide-on-win32-to-nt.html)
- [Windows Internals: Thread Synchronization Primitives (Medium)](https://medium.com/windows-os-internals/windows-internals-thread-synchronization-primitives-0b222b71f0ce) · [shift.click: Futex-like APIs](https://shift.click/blog/futex-like-apis/)
- [wepoll](https://github.com/piscisaureus/wepoll/blob/dist/wepoll.c) · [Len Holgate: \Device\Afd](https://lenholgate.com/blog/2023/04/adventures-with-afd.html) · [notgull: \Device\Afd](https://notgull.net/device-afd/)
- [NtCreateUserProcess (capt-meelo)](https://captmeelo.com/redteam/maldev/2022/05/10/ntcreateuserprocess.html) · [offensivedefence: NtCreateUserProcess](https://offensivedefence.co.uk/posts/nt-create-user-process/)
- *Windows Internals, 7th ed.* (Yosifovich, Ionescu, Russinovich, Solomon) — caps. System Service Dispatching, Memory Management, Processes/Threads, I/O System.
