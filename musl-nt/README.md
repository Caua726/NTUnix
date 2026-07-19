# musl-nt

Port experimental da musl 1.2.6 para executáveis PE x86-64 sobre NT. A API
vista pela aplicação continua sendo a API POSIX da musl; a fronteira de
syscalls traduz a ABI Linux para `kernel32` e `ntdll`. O binário final não usa
UCRT nem MSVCRT.

## Estado atual

- `libc-nt.a` e `crt0.o` compilam a partir da fonte oficial da musl;
- `ntposix-gcc` compila aplicações LP64 e gera PE/COFF;
- `hello`, a bateria da libc, o alocador e os testes de rede compilam e sao
  validados estaticamente (sem UCRT/MSVCRT); a execucao e' na VM;
- um BusyBox com mais de 80 applets de coreutils compila sem alteração no código-fonte;
- a bateria BusyBox cobre I/O, diretórios, metadata, `statfs`/`df`, relógio,
  regex, hashes, links, identidade, `fsync` e utilitários de texto;
- os PE produzidos importam apenas DLLs de base do NT.

O alvo é uma libc suficiente para o user space do NTUnix, não uma emulação
completa de Linux. `fork()` permanece deliberadamente indisponível.

## A questão de ABI

Windows x64 usa LLP64 (`long` tem 32 bits), enquanto musl x86-64 e aplicações
Unix usam LP64 (`long` tem 64 bits). Compilar a musl diretamente com MinGW
corromperia estruturas, varargs e interfaces públicas.

O pipeline resolve isso assim:

1. Clang compila cada unidade como `x86_64-unknown-linux-musl`, preservando
   tipos LP64 e `va_list` System V;
2. `sysv-coff-rewrite` marca funções e chamadas LLVM com a convenção
   `X86_64_SysV`;
3. `llc` baixa o IR para objeto Windows COFF;
4. o backend `nt/*.c`, compilado com MinGW, usa a ABI Win64 para chamar as DLLs;
5. `__nt_syscall`, declarado `sysv_abi`, é a ponte entre os dois mundos.

Isso mantém a ABI pública da musl e ainda produz um PE carregável diretamente
pelo NT.

## Build

Dependências no host:

- Clang, LLVM (`llc`, `llvm-config` e headers de desenvolvimento);
- toolchain x86-64 do mingw-w64;
- GNU Make e Bash;
- libvirt, QEMU e Samba para a VM de desenvolvimento (a execução é lá).

A fonte da musl não é vendorizada. O `make deps` da raiz a baixa para
`build/deps/musl-1.2.6`; use `MUSL_SRC` para outro local.

```sh
make -C musl-nt MUSL_SRC=/caminho/musl-1.2.6
make -C musl-nt MUSL_SRC=/caminho/musl-1.2.6 test
```

Artefatos principais:

```text
musl-nt/build/libc-nt.a
musl-nt/build/crt0.o
musl-nt/build/test/hello.exe
```

Para compilar uma aplicação:

```sh
musl-nt/ntposix-gcc programa.c -o programa.exe
```

O driver usa `MUSL_SRC=build/deps/musl-1.2.6` por padrão e aceita a mesma variável de
ambiente usada pelo Makefile.

## BusyBox

A fonte do BusyBox também fica fora do repositório. A configuração versionada
habilita o conjunto coreutils e o shell standalone/no-fork:

```sh
make busybox-nt \
  MUSL_SRC=/caminho/musl-1.2.6 \
  BUSYBOX_SRC=/caminho/busybox

make busybox-nt-check \
  MUSL_SRC=/caminho/musl-1.2.6 \
  BUSYBOX_SRC=/caminho/busybox
```

O primeiro alvo copia o resultado para `out/system/bin/busybox.exe`. O segundo
roda `objdump` sobre ele e falha se houver importação de UCRT/MSVCRT. A bateria
de runtime (`test/busybox-runtime.cmd`) é executada na VM, contra um kernel NT
de verdade.

O shell BusyBox executa applets marcados como `nofork` dentro do próprio
processo. Applets lançados diretamente (`busybox.exe cp ...`) funcionam
normalmente; pipelines ou scripts que exijam `fork()` ainda não são suportados.

## Raiz POSIX e caminhos

`NTUNIX_ROOT` define a árvore que corresponde a `/`. Sem a variável, a raiz é
inferida a partir do diretório do executável (ou removendo o sufixo
`system\bin`, quando presente).

```text
/etc/passwd
    ↓
%NTUNIX_ROOT%\etc\passwd
```

Caminhos relativos usam o diretório corrente do processo. Internamente todos
os caminhos são convertidos de UTF-8 para UTF-16.

## Semântica disponível

Implementações reais incluem:

- arquivos: `openat`, `read`, `write`, vetores, seek, truncate, dup, pipe e
  polling básico;
- diretórios e links: `getdents64`, mkdir, unlink/rmdir, rename, hard links,
  symlinks e reparse points;
- metadata: stat/statx, access, tempos e modos sintéticos;
- memória: mmap anônimo e de arquivo, mprotect, munmap e brk;
- tempo: relógios realtime/monotonic, nanosleep e gettimeofday;
- processo: pid/ppid, exec via `CreateProcessW`, wait e sinais de término;
- rede: sockets stream/datagram IPv4/IPv6, bind/connect/listen/accept,
  send/recv, mensagens vetorizadas, opções, shutdown e poll sobre Winsock;
- runtime: argv/env UTF-16 para UTF-8, auxv mínimo, TLS com `TlsAlloc`,
  setjmp/longjmp e cancelamento cooperativo.

Limitações intencionais da versão atual:

- `fork`/`vfork` retornam `ENOSYS`;
- uid/gid são 0; permissões POSIX são projetadas como modos sintéticos e
  chmod/chown são no-op;
- sinais têm uma emulação mínima;
- rede ainda não cobre ancillary data/SCM nem descoberta automática dos
  servidores DNS; `/etc/hosts` já é usado;
- não há `/proc`, namespaces ou outras interfaces específicas do Linux.

## Organização

```text
arch/x86_64/   overrides de syscall, pthread/TLS e crt
crt/           entrada PE, argv, ambiente e auxv
include/nt/    ABI Linux fixa e contrato interno
nt/            dispatcher e backends Win32/ntdll
override/      TLS, syscall cancellation e setjmp
tools/         pipeline LP64 → LLVM → COFF
test/          testes da libc e configuração/bateria BusyBox
```

A especificação e o mapa de implementação estão em
[`../docs/musl-nt-spec.md`](../docs/musl-nt-spec.md).
