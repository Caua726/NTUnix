# NTUnix — User Space Unix-like sobre o Kernel NT

## 1. Visão geral

NTUnix é uma proposta de sistema operacional construído sobre o kernel Windows NT, mas com um espaço de usuário próprio, inspirado na arquitetura e na filosofia de distribuições Linux.

O projeto não pretende ser apenas uma versão modificada visualmente do Windows. A ideia é manter os componentes que oferecem as principais vantagens técnicas do ecossistema Windows — kernel NT, drivers, NTFS, DirectX, WDDM e compatibilidade Win32 — enquanto grande parte do user space tradicional da Microsoft é substituída.

Em vez de Explorer, menu Iniciar, serviços convencionais, estrutura de diretórios Windows e gerenciamento fragmentado de software, o NTUnix teria desktop, serviços, runtime POSIX, sistema de pacotes e ferramentas próprias.

Em uma frase:

> NTUnix é um user space Unix-like construído sobre o kernel NT, com compatibilidade Windows integrada.

---

## 2. Objetivo principal

Criar um sistema que combine:

* compatibilidade com aplicativos e jogos Windows;
* suporte aos drivers existentes do Windows;
* arquitetura de user space semelhante a Linux e Unix;
* baixo consumo de RAM, CPU e armazenamento;
* serviços pequenos, modulares e declarativos;
* gerenciamento centralizado de pacotes e bibliotecas;
* possibilidade de recompilar software Unix e Linux para NT;
* controle integral sobre desktop, sessões, serviços e configuração.

O Windows tradicional deixaria de definir a experiência do sistema. Ele passaria a existir principalmente como uma camada de compatibilidade.

---

## 3. O que seria preservado do Windows

O NTUnix manteria os componentes necessários para utilizar o kernel NT e o ecossistema Windows.

### Núcleo

* `ntoskrnl.exe`;
* NT Executive;
* Object Manager;
* Memory Manager;
* I/O Manager;
* Process Manager;
* Security Reference Monitor;
* Plug and Play;
* gerenciamento de energia;
* HAL.

### Inicialização e processos estruturais

Dependendo da implementação e da versão do Windows usada como base:

* `bootmgr`;
* `winload`;
* `smss.exe`;
* `csrss.exe`;
* `wininit.exe`;
* `lsass.exe`;
* processos fundamentais de sessão e segurança.

### Hardware e armazenamento

* drivers Windows;
* WDM e WDF;
* WDDM;
* NTFS;
* NVMe, SATA, USB e HID;
* drivers de áudio, rede e GPU.

### Compatibilidade Windows

* `ntdll`;
* KernelBase;
* `kernel32`;
* `user32`;
* `gdi32`;
* Win32;
* WOW64;
* COM;
* RPC;
* DirectX;
* DXGI;
* WASAPI;
* Winsock;
* APIs criptográficas;
* Registro, quando necessário.

Nem todos esses componentes precisariam fazer parte do ambiente nativo do NTUnix, mas poderiam permanecer disponíveis para a camada de compatibilidade.

---

## 4. O que seria substituído

Grande parte do user space visível e administrativo do Windows seria removida, desativada ou isolada.

### Interface

* `explorer.exe`;
* desktop do Windows;
* barra de tarefas;
* menu Iniciar;
* pesquisa do Windows;
* central de notificações;
* aplicativo Configurações;
* gerenciador de arquivos;
* interfaces UWP desnecessárias.

### Serviços e aplicativos

* Widgets;
* Copilot;
* OneDrive;
* Edge integrado ao sistema;
* WebView2 onde não for necessário;
* aplicativos provisionados;
* telemetria não essencial;
* indexação convencional;
* serviços não utilizados;
* tarefas agendadas desnecessárias.

### Administração

* configuração principal pelo Registro;
* instalação individual de dependências por aplicativo;
* estrutura baseada em `Program Files`, `AppData` e múltiplos instaladores;
* dependência do Service Control Manager para serviços nativos do NTUnix.

---

## 5. Arquitetura geral

```text
┌───────────────────────────────────────────────┐
│ Aplicações NTUnix                             │
│ shell, terminal, arquivos, ferramentas e GUI  │
├───────────────────────────────────────────────┤
│ Bibliotecas compartilhadas                    │
│ libc, POSIX, GTK, Qt, SDL, X11 etc.           │
├───────────────────────────────────────────────┤
│ Serviços NTUnix                               │
│ initd, logd, deviced, networkd, sessiond      │
├───────────────────────────────────────────────┤
│ Runtime NT-POSIX                              │
│ paths, sinais, processos, FDs, IPC e pthreads │
├───────────────────────────────────────────────┤
│ Camada de compatibilidade Windows             │
│ Win32, COM, DirectX, SCM e componentes legados│
├───────────────────────────────────────────────┤
│ ntdll / Native API                            │
├───────────────────────────────────────────────┤
│ Kernel NT, Executive e drivers                │
└───────────────────────────────────────────────┘
```

Haveria dois ecossistemas principais de aplicações.

### Aplicações NTUnix

Compiladas especificamente para a ABI NT-POSIX:

```text
Aplicação NTUnix
        ↓
libc-nt / libntposix
        ↓
Native API ou APIs NT estáveis
        ↓
kernel NT
```

### Aplicações Windows

Executadas pela camada de compatibilidade Microsoft:

```text
Aplicação Windows
        ↓
Win32, COM, DirectX e DLLs Windows
        ↓
ntdll
        ↓
kernel NT
```

---

## 6. Runtime NT-POSIX

Uma das partes centrais do projeto seria uma biblioteca como:

```text
libntposix.dll
```

ou:

```text
libc-nt.so
```

Ela implementaria uma interface semelhante a POSIX sobre os mecanismos do NT.

### Exemplos de tradução

```text
open()            → NtCreateFile()
read()            → NtReadFile()
write()           → NtWriteFile()
mmap()            → NtCreateSection() + NtMapViewOfSection()
pthread_create()  → criação de threads NT
clock_gettime()   → timers e contadores NT
epoll()           → IOCP e objetos de espera
inotify           → ReadDirectoryChanges
futex             → WaitOnAddress ou keyed events
Unix sockets      → AF_UNIX, named pipes ou IPC próprio
```

A runtime também precisaria implementar conceitos que não possuem equivalência direta simples no NT:

* descritores POSIX;
* sinais;
* grupos de processos;
* sessões;
* `fork`;
* `exec`;
* PIDs virtuais ou coordenados;
* terminais PTY;
* permissões Unix;
* links simbólicos;
* `/proc`;
* `/sys`;
* `/dev`.

---

## 7. libc

O NTUnix poderia começar com uma libc pequena e adaptável, como musl ou newlib, em vez de portar imediatamente a glibc.

### Primeira fase

```text
newlib ou musl
        ↓
libntposix
        ↓
NT
```

### Fase posterior

Criar uma plataforma específica dentro da glibc:

```text
glibc/sysdeps/nt/
```

A glibc teria backends próprios para:

* arquivos;
* threads;
* memória;
* sinais;
* sockets;
* timers;
* carregamento dinâmico;
* sincronização;
* processos.

Portar a glibc não seria suficiente para rodar todos os softwares Linux. Programas que dependem diretamente de interfaces específicas do kernel Linux ainda precisariam ser adaptados.

---

## 8. Compatibilidade com software Linux

O NTUnix teria principalmente compatibilidade de código-fonte com Linux e Unix, não compatibilidade binária automática.

### Softwares provavelmente fáceis de portar

* Bash;
* Dash;
* coreutils;
* grep;
* sed;
* awk;
* Git;
* curl;
* OpenSSH;
* Vim;
* Neovim;
* Python;
* Lua;
* GCC;
* Clang;
* CMake;
* Meson;
* Ninja;
* OpenSSL;
* ncurses;
* SDL;
* muitas bibliotecas C e C++ portáveis.

### Softwares que precisariam de backends NT

* X.Org;
* Wayland;
* GTK;
* Qt;
* PipeWire;
* Mesa;
* libinput;
* NetworkManager;
* DBus;
* navegadores;
* ambientes de desktop.

Exemplos:

```text
ALSA       → WASAPI ou audiod próprio
udev       → Plug and Play NT
evdev      → Raw Input e HID
DRM/KMS    → WDDM, DXGI ou backend gráfico próprio
netlink    → RPC com serviços NTUnix
cgroups    → Job Objects
```

### Softwares profundamente dependentes de Linux

* systemd original;
* Docker;
* Podman;
* KVM;
* eBPF;
* nftables;
* ferramentas Btrfs;
* drivers Linux;
* compositores presos diretamente a DRM/KMS;
* softwares dependentes de namespaces e cgroups completos.

Eles exigiriam grandes adaptações ou reimplementações.

---

## 9. Compatibilidade com software Windows

O objetivo seria manter compatibilidade muito alta com:

* executáveis Win32;
* programas de 32 e 64 bits;
* jogos DirectX;
* ferramentas profissionais;
* launchers;
* software legado;
* drivers Windows.

Entretanto, não seria possível prometer compatibilidade absoluta se muitos componentes Microsoft fossem removidos.

Os casos mais difíceis seriam:

* anti-cheats em modo kernel;
* DRM;
* programas que instalam serviços ou drivers;
* aplicações que dependem do Explorer;
* extensões do shell;
* UWP e MSIX;
* softwares dependentes de APIs internas;
* programas que esperam serviços específicos do Windows;
* aplicativos dependentes do DWM e de comportamentos gráficos particulares.

Por isso, a camada de compatibilidade poderia manter uma instalação Win32 relativamente completa, mas isolada do ambiente principal.

---

## 10. Sistema de inicialização e serviços

O NTUnix teria um supervisor inspirado em systemd, s6, runit e launchd, mas desenvolvido especificamente para NT.

Nome provisório:

```text
initd
```

ou:

```text
ntd
```

Ele seria responsável por:

* iniciar serviços;
* supervisionar processos;
* resolver dependências;
* reiniciar serviços com falha;
* ativar serviços por socket;
* executar timers;
* coletar logs;
* criar sessões;
* aplicar limites de recursos;
* gerenciar targets ou perfis de inicialização.

### Exemplo de unidade

```ini
[Unit]
Description=Servidor de áudio
After=device.target
Requires=gpu.target

[Service]
ExecStart=/system/bin/audiod
Restart=on-failure
MemoryMax=64M
Socket=/run/audio.sock

[Install]
WantedBy=graphical.target
```

### Ferramenta de controle

```bash
ntctl start audiod
ntctl stop audiod
ntctl restart audiod
ntctl status audiod
ntctl enable audiod
ntctl logs audiod
```

### Serviços principais

```text
initd       — supervisor geral
logd        — logs centralizados
deviced     — dispositivos e Plug and Play
networkd    — rede
audiod      — áudio
sessiond    — usuários e sessões
displayd    — ambiente gráfico
packaged    — pacotes e atualizações
timed       — relógio e timers
powerd      — energia
```

Serviços nativos do Windows continuariam sendo gerenciados pelo SCM dentro da camada de compatibilidade.

---

## 11. Controle de recursos

O equivalente aproximado a cgroups seria construído principalmente sobre Job Objects.

Cada serviço poderia receber:

* limite de memória;
* prioridade;
* quota de CPU;
* afinidade;
* número máximo de processos;
* encerramento coletivo;
* contabilidade de recursos;
* política de reinicialização.

Exemplo:

```text
audiod.service
└── Job Object
    ├── audiod
    ├── codec-worker
    └── plugin-host
```

Quando o serviço fosse encerrado, todos os processos associados também seriam finalizados.

---

## 12. Estrutura de diretórios

O sistema apresentaria uma estrutura semelhante a Unix:

```text
/
├── boot/
├── system/
│   ├── bin/
│   ├── lib/
│   ├── drivers/
│   └── units/
├── bin/
├── sbin/
├── lib/
├── usr/
│   ├── bin/
│   ├── lib/
│   └── share/
├── etc/
├── run/
├── var/
│   ├── log/
│   ├── cache/
│   └── packages/
├── home/
├── dev/
├── proc/
├── sys/
├── opt/
└── windows/
    ├── system32/
    ├── programs/
    └── registry/
```

Fisicamente, os arquivos poderiam continuar armazenados em NTFS.

A runtime converteria:

```text
/etc/network.conf
```

em um caminho NT correspondente:

```text
\??\C:\NTUnix\etc\network.conf
```

Aplicações NTUnix não precisariam conhecer letras de unidade.

---

## 13. Registro e configuração

O Registro permaneceria disponível para aplicativos Windows, mas não seria o sistema principal de configuração do NTUnix.

```text
/etc/*.conf
        → configuração do NTUnix

Registro do Windows
        → compatibilidade Win32
```

O sistema nativo usaria arquivos declarativos, possivelmente em:

* TOML;
* YAML;
* JSON;
* formato próprio simples.

Isso permitiria:

* versionar configurações;
* copiar configurações entre máquinas;
* restaurar o sistema facilmente;
* construir instalações reproduzíveis.

---

## 14. Gerenciamento de pacotes

O NTUnix teria um gerenciador de pacotes central, por exemplo:

```text
ntpkg
```

### Exemplos

```bash
ntpkg install bash
ntpkg install xorg-server
ntpkg install firefox
ntpkg install gcc
ntpkg remove audiod
ntpkg update
ntpkg upgrade
```

### Estrutura de pacote

```text
firefox.ntpkg
├── manifest.toml
├── bin/
├── lib/
├── share/
├── units/
├── capabilities/
├── migrations/
└── signatures/
```

O gerenciador cuidaria de:

* dependências;
* bibliotecas compartilhadas;
* versões;
* assinaturas;
* atualizações;
* rollback;
* arquivos de configuração;
* ativação de serviços;
* permissões;
* transações.

---

## 15. Bibliotecas compartilhadas

Diferentemente da prática comum de aplicativos Windows carregarem todas as DLLs consigo, o NTUnix usaria bibliotecas compartilhadas instaladas centralmente.

```text
/usr/lib/libssl.so
/usr/lib/libpng.so
/usr/lib/libgtk.so
/usr/lib/libcurl.so
```

ou equivalentes PE:

```text
/usr/lib/libssl.dll
/usr/lib/libpng.dll
```

O gerenciador de pacotes garantiria versões e dependências.

O sistema poderia suportar:

* SONAME;
* versionamento de símbolos;
* múltiplas versões paralelas;
* RPATH;
* RUNPATH;
* manifests;
* namespaces de bibliotecas;
* pacotes isolados quando necessário.

Aplicativos Windows convencionais continuariam podendo carregar DLLs privadas.

---

## 16. Formato de executáveis

Existem duas possibilidades principais.

### PE nativo

Aplicações NTUnix seriam executáveis PE ligados à runtime NT-POSIX.

Vantagens:

* integração simples com o loader do Windows;
* menor esforço inicial;
* uso direto das ferramentas PE/COFF;
* compatibilidade melhor com debuggers e ferramentas Windows.

### ELF sobre NT

O NTUnix teria um loader ELF próprio.

Vantagens:

* maior compatibilidade com toolchains Linux;
* suporte natural a `.so`, SONAME e `ld.so`;
* possibilidade de preservar mais convenções Unix.

Desvantagens:

* loader próprio;
* relocations e TLS;
* tradução de syscalls;
* integração mais difícil com depuração e segurança do NT.

A primeira versão provavelmente deveria usar PE, deixando ELF para uma fase posterior.

---

## 17. Ambiente gráfico

O ambiente gráfico seria próprio, não apenas uma modificação do Explorer.

Possibilidades:

* X.Org como servidor gráfico principal;
* compositor Wayland portado para NT;
* protocolo gráfico próprio;
* sistema híbrido.

### X.Org

O núcleo do X.Org poderia ser reaproveitado, com um backend específico:

```text
hw/nt/
```

Esse backend cuidaria de:

* apresentação;
* input;
* cursores;
* modos de vídeo;
* múltiplos monitores;
* aceleração;
* buffers;
* integração com WDDM ou DXGI.

### Compositor próprio

O sistema poderia ter:

* gerenciamento tiled;
* workspaces;
* painel;
* launcher;
* notificações;
* lock screen;
* clipboard;
* drag-and-drop;
* protocolo de janelas.

DWM poderia inicialmente ser mantido como backend de compatibilidade para aplicações Win32. Uma substituição completa exigiria engenharia muito mais profunda.

---

## 18. DWM e compatibilidade gráfica

Forçar o encerramento do DWM é possível, mas isso não entrega automaticamente um caminho alternativo para compor janelas Win32.

O problema real seria obter e apresentar as superfícies gráficas dos aplicativos existentes.

Por isso, a arquitetura inicial mais segura seria:

```text
Ambiente NTUnix nativo
        → compositor próprio

Aplicativos Windows
        → Win32 + DWM
        → integração no ambiente NTUnix
```

O DWM poderia ser tratado como componente de compatibilidade, não como o desktop principal.

No futuro, poderia ser investigada uma ponte capaz de apresentar janelas Win32 dentro do compositor NTUnix.

---

## 19. Cygwin como referência

Cygwin já implementa uma parte importante dessa ideia:

* runtime POSIX sobre NT;
* Bash;
* ferramentas GNU;
* compilação de software Unix;
* estrutura `/usr`, `/etc`, `/var` e `/home`;
* sinais;
* `fork`;
* terminais;
* X.Org;
* gerenciador de pacotes.

A diferença é:

```text
Cygwin:
ambiente Unix dentro do Windows

NTUnix:
ambiente Unix-like como user space principal do NT
```

Cygwin seria uma importante referência técnica, mas não representa o sistema completo desejado.

---

## 20. Metas de eficiência

O projeto busca reduzir fortemente o peso do Windows tradicional.

### Protótipo mínimo

```text
RAM em repouso: 300–600 MB
Disco: 1–3 GB
```

Características:

* shell simples;
* terminal;
* arquivos;
* rede;
* poucos drivers;
* aplicações nativas;
* hardware específico.

### Sistema cotidiano

```text
RAM em repouso: 600 MB–1,2 GB
Disco: 3–8 GB
```

Características:

* áudio;
* aceleração gráfica;
* desktop completo;
* pacote de fontes;
* serviços persistentes;
* aplicações Unix recompiladas;
* camada Win32 parcial.

### Compatibilidade Windows ampla

```text
RAM em repouso: 1–2 GB
Disco: 8–20 GB
```

Características:

* DirectX completo;
* WOW64;
* COM;
* multimídia;
* instaladores;
* serviços Windows;
* ampla compatibilidade com jogos e aplicativos.

A meta de 500 MB de RAM e 2 GB de disco seria possível apenas para uma versão extremamente reduzida e específica.

---

## 21. Segurança

O NTUnix poderia aproveitar mecanismos nativos do NT:

* tokens de acesso;
* ACLs;
* integrity levels;
* AppContainers;
* Job Objects;
* object namespaces;
* isolamento de sessões;
* assinatura de código;
* Secure Boot;
* TPM.

Sobre isso, poderia implementar um modelo de capacidades para aplicações nativas.

Exemplo:

```toml
[Capabilities]
network = true
audio = true
camera = false
filesystem = [
  "/home/caua/Documents",
  "/tmp"
]
```

O gerenciador de pacotes e o supervisor aplicariam essas permissões no lançamento.

---

## 22. Atualizações e sistema reproduzível

O sistema poderia evitar atualizações tradicionais que alteram arquivos de forma imprevisível.

Possibilidades:

* partições A/B;
* snapshots;
* imagens imutáveis;
* atualizações transacionais;
* rollback automático;
* configuração declarativa;
* pacotes com hashes;
* builds reproduzíveis.

Exemplo:

```text
System A — versão ativa
System B — nova versão
```

Após uma atualização bem-sucedida, o bootloader trocaria para a nova imagem. Em caso de falha, retornaria à versão anterior.

---

## 23. Limitações técnicas

Os principais obstáculos seriam:

* APIs NT internas pouco documentadas;
* mudanças entre builds do Windows;
* dependência entre `ntoskrnl`, `ntdll`, `win32k`, CSRSS e DWM;
* incompatibilidade semântica entre POSIX e NT;
* implementação confiável de `fork`;
* sinais e terminais;
* integração com drivers;
* backend gráfico sem DWM;
* anti-cheats;
* serviços proprietários;
* licenciamento e distribuição;
* dificuldade de depuração;
* necessidade de manter componentes Microsoft sincronizados.

O projeto precisaria escolher cuidadosamente interfaces públicas e estáveis sempre que possível.

---

## 24. Limitações legais

O kernel NT e vários componentes necessários são proprietários da Microsoft.

O projeto não deveria distribuir diretamente:

* `ntoskrnl.exe`;
* DLLs Microsoft;
* drivers proprietários;
* imagens Windows modificadas;
* ISOs pré-ativadas.

O modelo mais seguro seria distribuir:

* código do NTUnix;
* runtime NT-POSIX;
* shell;
* serviços;
* sistema de pacotes;
* scripts;
* patches;
* ferramentas de construção.

O usuário forneceria uma ISO oficial do Windows e uma licença válida. O instalador construiria localmente a imagem NTUnix.

---

## 25. Estratégia de desenvolvimento

### Fase 1 — ambiente hospedado

Executar o NTUnix dentro de uma instalação Windows normal.

Criar:

* `libntposix`;
* `initd`;
* `ntctl`;
* `ntpkg`;
* estrutura Unix;
* shell;
* serviços básicos.

### Fase 2 — shell principal

Substituir o Explorer como shell da sessão.

Adicionar:

* painel;
* launcher;
* gerenciador de arquivos;
* workspaces;
* notificações;
* configurações;
* login próprio.

### Fase 3 — distribuição NT-POSIX

Criar toolchain:

```text
x86_64-unknown-ntposix
```

Portar:

* libc;
* Bash;
* coreutils;
* ncurses;
* OpenSSL;
* curl;
* Git;
* Python;
* GCC/Clang;
* CMake;
* Meson;
* Ninja.

### Fase 4 — desktop Unix

Portar:

* DBus;
* GLib;
* Fontconfig;
* FreeType;
* X.Org;
* GTK;
* Qt;
* window manager;
* desktop completo.

### Fase 5 — user space independente

Substituir mais serviços Windows por equivalentes NTUnix:

* rede;
* áudio;
* sessões;
* dispositivos;
* logs;
* atualizações;
* configuração.

### Fase 6 — compatibilidade isolada

Mover Win32, SCM e DWM para uma camada de compatibilidade separada, utilizada somente quando uma aplicação Windows for iniciada.

---

## 26. Primeiro protótipo viável

O primeiro protótipo não precisa substituir todo o Windows.

Ele poderia conter:

```text
NTUnix 0.1
├── initd
├── ntctl
├── logd
├── libntposix
├── /etc, /usr, /var e /home
├── shell Bash ou próprio
├── coreutils
├── gerenciador de pacotes
├── painel simples
├── launcher
├── gerenciador de arquivos
└── execução de aplicativos Windows
```

Tudo inicialmente poderia rodar sobre Win32, migrando gradualmente para Native API e serviços próprios.

O primeiro objetivo técnico deveria ser:

> Inicializar uma sessão sem Explorer, abrir um terminal NTUnix e controlar todos os serviços nativos por meio do `initd`.

---

## 27. Identidade do projeto

### Nome

**NTUnix**

### Significado

Combinação de:

```text
NT (kernel) + Unix (user space)
```

### Descrição curta

> Um user space Unix-like para o kernel NT.

### Descrição longa

> NTUnix é um sistema operacional experimental que utiliza o kernel NT, drivers e compatibilidade Windows como fundação, substituindo o user space tradicional por uma arquitetura modular inspirada em Linux e Unix.

### Slogan

> Unix philosophy. NT foundation.

Ou:

> O ecossistema Unix sobre a fundação NT.

---

## 28. Resultado final pretendido

O NTUnix seria um sistema em que o usuário pudesse fazer:

```bash
ntpkg install xorg-server
ntpkg install i3
ntpkg install firefox
ntpkg install steam

ntctl enable display-manager
ntctl start networkd
ntctl status audiod
```

O sistema inicializaria em um desktop próprio, utilizaria serviços próprios e organizaria seus arquivos e bibliotecas como uma distribuição Unix.

Aplicações Windows poderiam ser abertas normalmente quando necessário:

```bash
winrun Cyberpunk2077.exe
```

Aplicações Unix seriam recompiladas para a ABI NT-POSIX:

```bash
ntcc programa.c -o programa
```

O NTUnix não seria Linux, Windows convencional, Cygwin ou ReactOS.

Seria uma plataforma híbrida:

> kernel, drivers e compatibilidade do Windows; user space, ferramentas e filosofia de uma distribuição Unix.
