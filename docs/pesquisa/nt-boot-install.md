# NTUnix — Instalador live "Caminho C": aplicar a `install.wim` direto (estilo pacstrap)

> Pesquisa técnica. Objetivo: abandonar o `setup.exe` interativo da Microsoft e
> instalar o NTUnix aplicando a `install.wim` numa partição preparada à mão
> (`wimlib-imagex apply` / `dism /Apply-Image` + `bcdboot`), rodando de dentro de
> um WinPE mínimo usado só como "live USB". Base = Windows 11. O usuário traz a
> ISO oficial; **nós não redistribuímos bits da Microsoft**.

**Analogia central:** o WinPE é o nosso "ISO live do Arch"; o `wimlib apply` é o
`pacstrap`; o `bcdboot` é o `grub-install`/`bootctl install`; o `unattend.xml` +
`SetupComplete.cmd` são o nosso `arch-chroot ... configure`. A diferença honesta
(§3) é que o NT **obriga** um primeiro-boot pesado (specialize) que o Arch não tem.

---

## 0. Panorama do fluxo (o esqueleto)

```
[WinPE nosso]  ->  particiona (diskpart)  ->  formata ESP+Windows
             ->  wimlib apply install.wim  ->  bcdboot (torna bootável)
             ->  planta unattend.xml + SetupComplete.cmd offline
             ->  planta bypass Win11 no hive offline
             ->  reboot  ->  [1º boot NT: specialize + OOBE + nossos hooks]
             ->  cai no ntsession (shell nosso, sem explorer.exe)
```

Ponto que valida tudo: **a `install.wim` de uma ISO oficial já vem "generalized"**
(é assim que a Microsoft a distribui). Por isso, ao aplicá-la crua, o primeiro
boot dispara automaticamente o *specialize pass* + OOBE — não precisamos rodar
`sysprep`. É exatamente isso que torna o Caminho C viável.
(fonte: Microsoft "Capture and Apply Windows using a WIM file"; "Sysprep (Generalize)".)

---

## 1. Aplicar a `install.wim` manualmente

### 1.1. Particionamento GPT/UEFI

Layout padrão UEFI da Microsoft, na ordem física: **System (ESP) → MSR → Windows
→ Recovery**. (fonte: "UEFI/GPT-based hard drive partitions".)

| Partição | Tipo | FS | Tamanho | Letra (no WinPE) |
|---|---|---|---|---|
| ESP (EFI System Partition) | `efi` | **FAT32** | 260–500 MB (mín. 200 MB p/ setor 512e, 300 MB p/ 4Kn) | `S:` |
| MSR (Microsoft Reserved) | `msr` | — (sem letra, sem FS) | **16 MB** | — |
| Windows | `primary` | **NTFS** | ≥ 20 GB (real: ≥ 120 GB) | `W:` |
| Recovery (opcional no v0) | `primary` (type `DE94BBA4-06D1-4D40-A16A-BFD50179D6AC`) | NTFS | 300–990 MB | `R:` |

Notas importantes (fonte: doc UEFI/GPT + "Capture and Apply"):
- Use **W** para a partição Windows (não X — `X:` é reservado ao WinPE). Após o
  reboot o NT reatribui a Windows como `C:`.
- A ESP **precisa** ser FAT32 (firmware UEFI só lê FAT). A Windows **precisa** ser NTFS.
- MSR não recebe letra nem dado; é reserva de gestão de partição. Não é
  estritamente necessária pra bootar, mas o layout "correto" a inclui — mantê-la
  evita surpresas com ferramentas MS futuras (WinRE, BitLocker, feature updates).
- No v0 do NTUnix a partição Recovery é dispensável (não usamos WinRE). Se
  quiser suporte a "reset"/BitLocker depois, adicione-a **logo após** a Windows.

**Script diskpart** (`CreatePartitions-UEFI.txt`, modelo derivado da amostra MS):

```
rem === CreatePartitions-UEFI.txt ===
select disk 0
clean
convert gpt
rem -- ESP --
create partition efi size=300
format quick fs=fat32 label="System"
assign letter=S
rem -- MSR --
create partition msr size=16
rem -- Windows --
create partition primary
shrink minimum=990          rem (só se for criar Recovery ao final)
format quick fs=ntfs label="NTUnix"
assign letter=W
rem -- Recovery (opcional) --
create partition primary
format quick fs=ntfs label="Recovery"
assign letter=R
set id="de94bba4-06d1-4d40-a16a-bfd50179d6ac"
gpt attributes=0x8000000000000001
exit
```

Executa-se com: `diskpart /s X:\CreatePartitions-UEFI.txt` (X = mídia do WinPE).

### 1.2. Aplicar a imagem

A `install.wim` da ISO tem várias edições (índices). `wimlib-imagex info install.wim`
lista. Escolha o índice (ex.: Pro = geralmente 6; Home = 1 — varia).

**Opção A — de dentro do WinPE (recomendada), com DISM (já vem no WinPE):**
```cmd
dism /Apply-Image /ImageFile:D:\sources\install.wim /Index:6 /ApplyDir:W:\
```

**Opção A' — de dentro do WinPE, com wimlib (se embutirmos `wimlib-imagex.exe`):**
```cmd
wimlib-imagex apply D:\sources\install.wim 6 W:\
```
(Sintaxe: `wimlib-imagex apply WIMFILE [IMAGE] TARGET`. Se a `install.esd` for
comprimida, wimlib aplica direto também. Para WIM dividido em `.swm`:
`--ref="D:\sources\install*.swm"`.) (fonte: manpage `wimlib-imagex-apply`.)

**Opção B — de dentro do Linux, wimlib escrevendo NTFS via libntfs-3g:**
```bash
wimlib-imagex apply install.wim 6 /dev/nvme0n1p3    # alvo = volume NTFS cru
```
No modo "volume NTFS" (UNIX), wimlib usa libntfs-3g e **preserva** security
descriptors (dono/ACL), named data streams, short names, etc. — coisas que ele
**não** consegue preservar ao extrair para um diretório comum no Linux.
(fonte: manpage wimlib.)

**Solidez:** A e A' são o caminho batido (a própria MS documenta `dism
/Apply-Image` + `bcdboot`; ver `ApplyImage.bat` da MS na §2). B é tecnicamente
possível e sedutor (instalar 100% do Linux), mas **arriscado**: (i) o `bcdboot`
não roda no Linux, então o boot teria de ser montado à mão ou num segundo passo
no Windows; (ii) qualquer divergência de ACL/atributo NTFS-3G vs NT pode causar
falha sutil no specialize. **Recomendação NTUnix:** usar **A (DISM no WinPE)** no
v0 — é o que a Microsoft assina embaixo. Manter B como pesquisa futura para um
instalador 100%-Linux (bootstrap sem Windows nenhum), mas não é o caminho do 0.1.

**Ponto crítico:** `wimlib apply` / `dism apply` **NÃO tornam o volume bootável**.
A própria manpage do wimlib avisa: *"to actually boot Windows … you also need to
mark the partition as bootable and set up various boot files, such as \BOOTMGR
and \BOOT\BCD"*. Isso é o §2.

---

## 2. Tornar bootável — `bcdboot`

Comando canônico (rodando de dentro do WinPE, usando o `bcdboot` **da imagem que
acabamos de aplicar**, não o do WinPE):

```cmd
W:\Windows\System32\bcdboot W:\Windows /s S: /f UEFI
```

O que ele faz (fonte: "BCDBoot Command-Line Options", "BCD System Store Settings
for UEFI"):
1. Copia um conjunto pequeno de *boot-environment files* da imagem Windows para a
   ESP, criando `\EFI\Microsoft\Boot\` — incluindo o **`bootmgfw.efi`** (o boot
   manager EFI da Microsoft) e o BCD.
2. Cria um **BCD store** novo na ESP, a partir do template
   `%WINDIR%\System32\Config\BCD-Template`, com a entrada do Windows Boot Manager.
3. Em UEFI, por padrão **adiciona uma entrada na NVRAM do firmware** apontando
   para o `bootmgfw.efi` — e a coloca em primeiro na ordem de boot.

Opções relevantes:
- `/f UEFI` → só arquivos UEFI, cria `\Efi\Microsoft\Boot`. (`/f ALL` faz UEFI+BIOS;
  `/f BIOS` cria `\Boot`.) Se usar `/f`, **tem** que usar `/s`.
- `/s S:` → escreve os boot files na partição S em vez de detectar a ESP sozinho.
- `/l pt-br` → locale do BCD.
- `/c` → começa um BCD store do zero, ignorando entradas antigas.

**Armadilha do `/s` (importante):** *"If the /s option is used, then [the NVRAM]
entry is not created. Instead, BCDBoot relies on the default firmware settings …
\efi\boot\bootx64.efi in the ESP."* Ou seja: com `/s`, o `bcdboot` copia para
`\EFI\Microsoft\Boot\bootmgfw.efi` mas **não grava a NVRAM** — depende do
fallback removível `\EFI\Boot\bootx64.efi`. Consequências:
- **Instalando na máquina-alvo real** (disco que vai ficar nela): prefira rodar
  **sem `/s`** — `W:\Windows\System32\bcdboot W:\Windows /f UEFI` — para que ele
  detecte a ESP montada **e grave a entrada NVRAM**, garantindo boot mesmo sem
  fallback. (Requer a ESP montada com letra; no WinPE atribua a letra e deixe-o
  achar.)
- **Instalando num disco que vai bootar noutra máquina / VM / pendrive**: use
  `/s S:` **e** copie o fallback: `copy S:\EFI\Microsoft\Boot\bootmgfw.efi
  S:\EFI\Boot\bootx64.efi` (crie `\EFI\Boot`), para o firmware achar via caminho
  removível padrão.

**Dá pra montar o BCD à mão (sem bcdboot)?** Sim, com `bcdedit /createstore`,
`bcdedit /create {bootmgr}`, `/set device partition=S:`, `/create /d "NTUnix"
/application osloader`, etc. — mais copiar `bootmgfw.efi` na mão. **Vale?** Não
para o v0: é dezenas de comandos frágeis, versão-dependentes, e o `bcdboot` já
faz tudo isso corretamente a partir do `BCD-Template` da própria imagem.
**Recomendação:** usar `bcdboot`. Montar BCD à mão só se um dia quisermos um
gerenciador de boot próprio (aí sim faz sentido gerar o store nós mesmos).

**Alternativas fora do bcdboot:** em teoria o NTUnix poderia ignorar o
`bootmgr`/BCD da MS e apontar a NVRAM (via `efibootmgr` no Linux, ou
`bcdedit`/protocolo EFI) direto para `\Windows\System32\winload.efi` com um
BCD mínimo — mas `winload.efi` **exige** um BCD válido que descreva a partição do
SO; não há como bootar o kernel NT sem `bootmgr` + BCD. Então o BCD é inevitável;
a única escolha é "gerado por bcdboot" (sólido) vs "montado à mão" (arriscado).

---

## 3. Primeiro boot / specialize — o que o NT OBRIGA

Aplicar uma `install.wim` generalizada e bootar **dispara obrigatoriamente**,
antes de qualquer sessão de usuário:

1. **`specialize` pass** — roda no primeiro boot de uma imagem generalizada
   (fonte: "How Configuration Passes Work", "Sysprep Generalize"). Nele o NT:
   - **gera um novo SID de máquina** (a imagem generalizada teve o SID removido);
   - **re-enumera Plug and Play** (instala drivers do hardware real — o motivo de
     a imagem ser portável); com `PersistAllDeviceInstalls=true` no unattend dá
     pra pular parte disso, mas em bare-metal você quer a enumeração real;
   - aplica **licenciamento/ativação**, nome de máquina, configs específicas da
     máquina, re-arm de coisas de OOBE;
   - processa a seção `<settings pass="specialize">` do `unattend.xml`.
2. Reinício automático.
3. **`oobeSystem` pass** — processa `<settings pass="oobeSystem">`; é onde entram
   conta local, autologon, `SkipMachineOOBE`/`SkipUserOOBE`, região/teclado.
4. **`SetupComplete.cmd`** roda como **SYSTEM**, ao final do setup, em torno do
   primeiro logon — é o gancho ideal para plantar o shell e desligar serviços
   (o NTUnix já usa isso hoje).

**Onde plantar cada hook no Caminho C** (sem mídia de boot com `autounattend.xml`):
- `unattend.xml` → **`W:\Windows\Panther\unattend.xml`** na imagem já aplicada.
  O NT lê esse caminho automaticamente no primeiro boot (é a localização offline
  "implícita"). *No fluxo antigo (setup.exe) o arquivo ficava na raiz da mídia
  como `autounattend.xml`; no Caminho C não há setup.exe lendo a mídia, então
  ele vai pra Panther dentro do disco.*
- `SetupComplete.cmd` → **`W:\Windows\Setup\Scripts\SetupComplete.cmd`** (mesmo
  lugar de hoje; só copiamos pós-apply em vez de injetar na WIM).
- Alternativa a `unattend`/`SetupComplete`: registrar um **`RunOnce`** no hive
  offline, ou um `FirstLogonCommands` no `oobeSystem`. Menos limpo; ficar com os
  dois canônicos.

**Analogia honesta com `systemd-firstboot`:**
- `systemd-firstboot` é **opcional e leve**: seta locale, hostname, timezone,
  senha de root — e você pode pré-preencher tudo offline e ele nem roda.
- O `specialize` do NT é **obrigatório e pesado**: gera SID, instala drivers,
  ativa, reinicia. Você **não** consegue pré-configurar 100% offline e pular o
  primeiro boot; sempre haverá uma "primeira inicialização especial" que você só
  **direciona** (via `unattend` + `SetupComplete`), não elimina.
- **Consequência de design pro NTUnix:** o "pacstrap" (apply) é reproduzível e
  offline, mas o "primeiro boot" nunca será tão nu quanto um `arch-chroot`.
  Aceitar isso: o instalador termina com "reboot e deixa o NT se especializar"; o
  ambiente NTUnix de fato só existe *depois* do specialize+OOBE, guiado pelos
  nossos hooks. É inevitável — é o preço de reusar o kernel NT.

---

## 4. WinPE mínimo — o "live USB" do instalador

O WinPE é um Windows enxuto que boota num **RAM disk (`X:`)**: copia o `boot.wim`
para a RAM e cria um scratch gravável; **tudo é volátil** (reinicia perde
drivers, letras, registro) e há **reboot automático após 240 h** de uso contínuo
(era 72 h até a 1709) — irrelevante para um instalador que roda minutos.
(fonte: "Windows PE (WinPE)"; WinPE RAM notes.)

**De onde vem o `boot.wim` (sem redistribuir MS)?** Duas rotas:

- **Rota 1 — reusar o `boot.wim` da ISO do usuário (recomendada).** A ISO já
  contém `sources\boot.wim` com **índice 1 = WinPE** e **índice 2 = Setup**.
  Montamos o índice 1, largamos nosso instalador + `winpeshl.ini`, e repacotamos.
  É o mesmo espírito do `make-iso.sh` atual, mas em vez de deixar o `setup.exe`
  rodar, sequestramos o shell do WinPE para rodar **nosso** script. Zero bits MS
  redistribuídos: tudo sai da ISO do próprio usuário, na máquina dele.
- **Rota 2 — `copype` do ADK.** Se o usuário instalar o *Windows ADK* + *WinPE
  add-on*, `copype amd64 C:\WinPE_amd64` gera um WinPE limpo; montamos com
  `dism /Mount-Image`, customizamos, `MakeWinPEMedia`. Mais limpo e atualizável,
  mas **depende do ADK instalado**. (fonte: "WinPE: Mount and Customize".)

**Recomendação NTUnix:** Rota 1 no v0 (o usuário já traz a ISO; não exige ADK).
Rota 2 como opção "avançada" documentada.

**Customização (montando o `boot.wim` índice 1):**
```cmd
dism /Mount-Image /ImageFile:boot.wim /Index:1 /MountDir:C:\mnt
rem  -> copiar nossos binários: C:\mnt\NTUnix\  (ntsetup.exe, wimlib-imagex.exe, scripts)
rem  -> escrever winpeshl.ini / startnet.cmd
dism /Unmount-Image /MountDir:C:\mnt /Commit
```
(No Linux dá pra fazer o equivalente com `wimlib-imagex update`/`extract` — o
`make-iso.sh` já usa `wimlib-imagex update` para injetar árvore na WIM.)

**Auto-arranque do nosso instalador — duas formas:**

- `startnet.cmd` (default do WinPE roda só `wpeinit`). Substituir por:
  ```cmd
  wpeinit
  X:\NTUnix\ntsetup.exe
  ```
- ou `winpeshl.ini` (substitui o shell padrão `cmd.exe`), formato:
  ```ini
  [LaunchApps]
  %SYSTEMDRIVE%\NTUnix\ntsetup.exe
  ```
  Sem `winpeshl.ini`, o WinPE roda `cmd.exe` que roda `startnet.cmd`. Com ele, o
  WinPE lança direto nossa app. Se o app fechar, o WinPE **reinicia** — bom para
  o fluxo "instalou → reboot". (fonte: docs WinPE startnet/winpeshl.)

**Componentes opcionais (`.cab` do ADK)** — só se precisarmos: `WinPE-WMI.cab`,
`WinPE-Scripting.cab` (WSH), `WinPE-PowerShell.cab`, `WinPE-NetFx.cab`,
`WinPE-HTA.cab`. Adicionados via `dism /Add-Package`. **Recomendação:** um
`ntsetup.exe` nativo (Win32/PE, como o resto do NTUnix já é) **não precisa de
nenhum deles** — DISM/diskpart/bcdboot já estão no WinPE base. Mantenha o WinPE
mínimo (menor RAM disk, boot mais rápido). Só puxe `.cab` se um dia o instalador
virar PowerShell.

**Limitações a lembrar:** volátil (nada persiste entre reboots), RAM disk exige
RAM ≥ tamanho do boot.wim + scratch + apps, sem serviços pesados, e é
firmware-sensível (precisa bootar UEFI x64 — casar com o alvo).

---

## 5. Substituição do shell da sessão no sistema-alvo

**Mecanismo.** O NT lê o valor `Shell` em
`HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon` (por-máquina; há
também por-usuário em `HKCU\...\Winlogon\Shell`). Fluxo no logon interativo:
`winlogon.exe` → `userinit.exe` → lê `Shell` → lança o processo (por padrão
`explorer.exe`). O NTUnix já grava esse valor apontando pro `ntsession.exe`
(no `SetupComplete.cmd`). (fonte: forums/refs Winlogon Shell.)

**Sessão 0 vs sessões interativas.** Desde o Vista há **Session 0 Isolation**:
serviços rodam na sessão 0 (sem UI); o usuário loga na sessão **1+**. O
`ntsession` roda na sessão interativa do usuário (é o `Shell` do Winlogon), então
tem desktop/janelas normalmente. O `initd` e serviços NTUnix, se rodarem como
serviço/SYSTEM, cairão na sessão 0 (sem UI) — o que é **correto** para daemons.
Cuidado: um processo em sessão 0 **não** deve tentar abrir janela pro usuário.

**O que QUEBRA sem `explorer.exe`** (explorer é o *shell*, não só um file manager):
- **Barra de tarefas, menu Iniciar, área de notificação (system tray)** — somem.
  `Shell_NotifyIcon` (ícones de tray) não tem onde desenhar → apps que dependem
  de tray (muitos updaters, antivírus UI, VPNs) ficam "invisíveis" ou falham.
- **Desenho do desktop/wallpaper e ícones da área de trabalho** — o explorer que
  os pinta; sem ele, tela sem ícones.
- **AutoPlay / notificações toast / balões**, **drag-and-drop entre janelas do
  shell**, e integrações de **shell extensions** que assumem o host explorer.
- Partes de **Configurações**/**Windows Security** podem agir de forma
  inconsistente sem o shell completo.

**O que CONTINUA funcionando** (não depende do explorer estar rodando):
- **`SHGetKnownFolderPath` / `SHGetFolderPath`** — resolvem caminhos via
  registro/API (KnownFolders), **retornam normalmente** mesmo sem explorer. As
  *known folders* (Documents, Downloads, AppData…) existem no disco e no perfil.
- `CreateProcess`, todo o Win32 comum, COM, o namespace do shell via API
  (`IShellFolder`) — funcionam; só não há a *janela* do explorer hospedando.
- Perfis de usuário, variáveis de ambiente, associações de arquivo no registro
  (a *resolução* funciona; o *duplo-clique no desktop* é que não existe sem tray).

**Como o `ntsession` deve se comportar** (recomendações):
- Rodar na sessão interativa, **manter-se vivo** (já faz: ressuscita o terminal).
- Exportar `NTUNIX_ROOT`/PATH e garantir o `initd` (já faz).
- Aceitar que **não há tray nem Start** por ora; o "desktop" é o terminal. Isso
  está alinhado com a meta §26 ("inicializar sessão sem Explorer, abrir terminal,
  controlar serviços via initd").
- **Não** confiar em Shell_NotifyIcon nem em broadcasts de shell. Se um dia
  quisermos tray/notificações, o `ntsession` terá de **hospedar a própria área de
  notificação** (criar a janela `Shell_TrayWnd`/`TrayNotifyWnd` que os apps
  procuram) — é o que shells alternativos (Cairo, bbLean) fazem. Alternativa
  arriscada: lançar `explorer.exe` seletivamente (mas ele reassume desktop/tray e
  atrapalha o modelo NTUnix). **Recomendação:** nada de explorer; evoluir tray
  próprio quando necessário.
- **Rede de segurança obrigatória:** se `ntsession.exe` crashar/faltar, o usuário
  fica **sem shell** (tela preta). Mitigações: (a) o autologon do unattend + um
  `Shell` que, se falhar, permita recuperação; (b) manter uma via de escape —
  Safe Mode ainda carrega, e `Ctrl+Shift+Esc` abre o Task Manager (que consegue
  "Executar nova tarefa" → `cmd`/`regedit`) mesmo sem shell. Documentar como
  restaurar `Shell=explorer.exe` via WinPE/regedit offline.

---

## 6. Bypass dos requisitos do Win11 (TPM/SecureBoot/CPU/RAM)

**Insight decisivo do Caminho C:** o gate de TPM/SecureBoot/CPU/RAM é imposto
pelo **`setup.exe`** (o *compatibility appraiser* do instalador). Ao **aplicar a
`install.wim` direto com wimlib+bcdboot, pulamos o `setup.exe` inteiro** → o
check de instalação **simplesmente não roda**. Ou seja, o bypass principal vem
**de graça** no Caminho C. (fonte: threads de bypass; "DISM apply works without
doing checks".)

Porém, dois pontos ainda checam e valem "cinto e suspensório":
1. **OOBE do primeiro boot** pode reavaliar hardware em alguns builds.
2. **Feature updates futuras** (upgrade in-place) reavaliam TPM/CPU.

Por isso, plantar as chaves **na imagem já aplicada (hive offline)**:

```cmd
rem  -- carrega o SYSTEM hive da imagem aplicada --
reg load HKLM\OFF W:\Windows\System32\config\SYSTEM

rem  -- LabConfig: bypass de instalação/OOBE --
reg add HKLM\OFF\Setup\LabConfig /v BypassTPMCheck        /t REG_DWORD /d 1 /f
reg add HKLM\OFF\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f
reg add HKLM\OFF\Setup\LabConfig /v BypassRAMCheck        /t REG_DWORD /d 1 /f
reg add HKLM\OFF\Setup\LabConfig /v BypassCPUCheck        /t REG_DWORD /d 1 /f
reg add HKLM\OFF\Setup\LabConfig /v BypassStorageCheck    /t REG_DWORD /d 1 /f

rem  -- MoSetup: permite UPGRADES futuros em HW sem TPM/CPU suportado --
reg add HKLM\OFF\Setup\MoSetup /v AllowUpgradesWithUnsupportedTPMOrCPU /t REG_DWORD /d 1 /f

reg unload HKLM\OFF
```

Notas:
- `HKLM\SYSTEM\Setup\LabConfig` é lido pelo appraiser; no Caminho C a rota de
  instalação já não passa por ele, mas plantá-lo cobre OOBE/reexecuções.
- `Setup\MoSetup\AllowUpgradesWithUnsupportedTPMOrCPU=1` é a chave *persistente*
  que a MS documenta para **upgrades** subsequentes em hardware não suportado.
- **Solidez:** plantar no hive offline é robusto e reproduzível (não depende de
  digitar `Shift+F10`/regedit no meio do setup). **Risco:** nenhum funcional; o
  "risco" é político/de suporte (instalar em HW não suportado é sem garantia MS —
  já é premissa do projeto).
- **Alternativa mais radical** (não recomendada no v0): editar o `boot.wim`
  índice 2 (Setup) para neutralizar `appraiserres.dll` — desnecessário, já que
  nem usamos o setup.exe.

**Recomendação NTUnix:** manter o bypass **no hive offline** (acima), e **remover**
a duplicação atual do `autounattend.xml` (que grava LabConfig no `HKLM\SYSTEM\Setup`
via `RunSynchronous` do specialize) — no Caminho C o unattend de specialize ainda
roda, mas o hive offline já resolve antes; consolidar num lugar só evita
divergência.

---

## 7. Solidez vs risco — resumo por frente

| Frente | Sólido | Arriscado / cuidado |
|---|---|---|
| 1. Apply | `dism /Apply-Image` no WinPE; imagem já generalizada | wimlib-from-Linux (B): bcdboot não roda no Linux; ACLs NTFS-3G |
| 2. bcdboot | `bcdboot W:\Windows /f UEFI` grava NVRAM | usar `/s` sem fallback `bootx64.efi` → não boota; BCD à mão |
| 3. 1º boot | unattend em `Panther`, SetupComplete em `Setup\Scripts` | specialize é **inevitável**; não dá "chroot puro" |
| 4. WinPE | reusar `boot.wim` da ISO; `winpeshl.ini`→ntsetup | volátil; casar firmware UEFI x64; RAM suficiente |
| 5. Shell | `Winlogon\Shell=ntsession`; SHGetKnownFolderPath OK | sem tray/Start; crash do shell = tela preta (ter escape) |
| 6. Bypass | `setup.exe` pulado = check some; hive offline p/ OOBE/upgrade | só político/suporte (HW não suportado) |

---

## 8. Recomendação de arquitetura pro instalador NTUnix

1. **`ntsetup.exe` nativo** (mesmo stack Win32/PE do resto do repo), embarcado num
   WinPE derivado da **própria ISO do usuário** (reusa `boot.wim` índice 1),
   auto-lançado por `winpeshl.ini`.
2. O `ntsetup` orquestra: `diskpart /s` (particiona) → `dism /Apply-Image`
   (aplica a edição escolhida) → planta `unattend.xml`/`SetupComplete.cmd` +
   árvore `\NTUnix` → `reg load`/bypass Win11 offline → `bcdboot W:\Windows /f UEFI`
   → strip offline opcional (o `strip.list` atual dá pra aplicar por delete na
   árvore `W:\` já aplicada, em vez de na WIM) → `wpeutil reboot`.
3. Manter `make-iso.sh` como **gerador da mídia do instalador** (empacota o WinPE
   sequestrado), mas o *core* da instalação migra de "setup.exe + hooks" para
   "apply + bcdboot" dentro do `ntsetup`.
4. O primeiro boot cai no specialize/OOBE guiado pelos hooks → `ntsession`.

---

## Fontes

- Microsoft Learn — Capture and Apply Windows using a WIM file:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/capture-and-apply-windows-using-a-single-wim?view=windows-11
- Microsoft Learn — UEFI/GPT-based hard drive partitions:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/configure-uefigpt-based-hard-drive-partitions?view=windows-11
- Microsoft Learn — BCDBoot Command-Line Options:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdboot-command-line-options-techref-di?view=windows-11
- Microsoft Learn — BCD System Store Settings for UEFI:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcd-system-store-settings-for-uefi?view=windows-11
- Microsoft Learn — Sysprep (Generalize) a Windows installation:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/sysprep--generalize--a-windows-installation?view=windows-11
- Microsoft Learn — How Configuration Passes Work:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/how-configuration-passes-work?view=windows-11
- Microsoft Learn — Windows PE (WinPE) intro / limitações:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-intro?view=windows-11
- Microsoft Learn — WinPE: Mount and Customize:
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/winpe-mount-and-customize?view=windows-11
- Microsoft Learn — OEM deployment / sample scripts (CreatePartitions-UEFI, ApplyImage.bat):
  https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/oem-deployment-of-windows-desktop-editions?view=windows-11
- wimlib — `wimlib-imagex apply` manpage (aplicar WIM, NTFS-3G, aviso "not bootable"):
  https://manpages.org/wimlib-imagex-apply  •  https://wimlib.net/
- Bypass Win11 (LabConfig / MoSetup / DISM apply sem check) — Tom's Hardware / BleepingComputer / woshub:
  https://www.tomshardware.com/how-to/bypass-windows-11-tpm-requirement  •
  https://www.bleepingcomputer.com/news/microsoft/how-to-bypass-the-windows-11-tpm-20-requirement/  •
  https://woshub.com/windows-11-unsupported-hardware-no-tpm-secure-boot/
- Winlogon Shell replacement / shells alternativos (o que quebra sem explorer):
  https://windowsforum.com/threads/how-to-replace-winlogon-shell.213611/  •
  https://www.tenforums.com/customization/162697-can-no-longer-change-default-shell-explorer-exe-custom.html
