# Estrutura da imagem do Windows 11 (24H2+) e strip do user space

> Frente de pesquisa NTUnix. Objetivo: dominar a estrutura da imagem do Win11
> para stripar o user space da Microsoft com segurança e agressividade
> calibrada, mantendo kernel/drivers/Win32/serviços de boot/logon/servicing.
>
> **Estado do repo hoje** (`build/strip.list`, `build/SetupComplete.cmd`,
> `build/autounattend.xml`, `build/make-iso.sh`): abordagem **conservadora e
> correta na filosofia** (raw-delete só do óbvio; serviços/telemetria
> desligados no primeiro boot), mas com **três defeitos concretos** que
> precisam de correção antes de subir a agressividade — ver §5, §3 e §1.6.
>
> Tudo aqui é hipótese a validar em VM. As fontes reais estão em §11.

---

## 0. TL;DR

- A imagem instalável é `sources/install.wim` (ou `install.esd`), com **um
  índice por edição** (Home/Pro/Education/...). O que boota o Setup é
  `sources/boot.wim` (**índice 1 = WinPE base**, **índice 2 = Windows Setup**).
- **Remover em nível de arquivo (raw delete)** só é seguro para um punhado de
  coisas (Edge, OneDrive, wallpapers extras, drivers de 3os). **Apps
  provisionados NÃO devem ser removidos por raw-delete da pasta**: o jeito certo
  é **desprovisionar** (`/Remove-ProvisionedAppxPackage`), que também escreve a
  chave `Deprovisioned` que impede o app de voltar em feature update.
- **O NTUnix compila no Linux com wimlib — não tem DISM.** Isso muda tudo no
  item "apps provisionados": DISM não roda no Linux/Wine de forma confiável.
  A saída robusta é **desprovisionar online no primeiro boot** (PowerShell no
  `SetupComplete.cmd`), que remove arquivos **e** cria a chave `Deprovisioned`.
- **Bug atual do NTUnix (grave):** o bypass de TPM/SecureBoot/RAM está no passo
  **`specialize`**, mas o gate de hardware roda **antes**, no **`windowsPE`**.
  Em hardware sem TPM, o `autounattend.xml` atual **não passa** num clean
  install de 24H2. O bypass precisa ir para o passo `windowsPE`
  (`Microsoft-Windows-Setup/RunSynchronous`) **e/ou** na registry do `boot.wim`.
- **24H2 específico:** depois de mexer no `install.wim`, é preciso **copiar
  `setuphost.exe` do `boot.wim\sources` para `\sources` da mídia**, senão o
  Setup falha por mismatch de binário. wimlib ≥1.14 tem um bug de detecção de
  arquitetura em imagens 24H2 capturadas — usar mídia oficial, não recapturar.
- **Nunca remover:** `SystemApps` (LogonUI/ShellExperienceHost), `WinSxS`
  (component store/servicing), Defender (remoção offline corrompe servicing),
  WebView2 (instaladores/Office/Teams dependem), servicing stack, CBS/TrustedInstaller.

---

## 1. Estrutura da ISO / imagem do Windows 11

### 1.1 Layout da ISO (mídia de instalação)

```text
\
├── autounattend.xml         (opcional — Setup lê da raiz do boot media)
├── boot\                    bootmgr de BIOS + etfsboot.com (El Torito BIOS)
├── efi\                     bootmgfw/bootmgr EFI + efisys.bin (El Torito UEFI)
│   └── microsoft\boot\      efisys.bin / efisys_noprompt.bin, BCD, fontes
├── sources\
│   ├── boot.wim             WinPE + Windows Setup (o que dá boot)
│   ├── install.wim | .esd   a(s) imagem(ns) do SO por edição
│   ├── setup.exe            entry point do Setup (chama SetupHost.exe)
│   ├── setuphost.exe        (24H2+) host do novo Setup — ver §1.6
│   ├── appraiserres.dll     checagem de requisitos (compat appraiser) — §1.5
│   ├── *.dll, lang.ini, ei.cfg, pid.txt ...
│   └── sxs\                 payloads de servicing usados pelo Setup
├── support\, upgrade\, ...
└── setup.exe                (cópia na raiz)
```

- **BIOS boot:** `boot\etfsboot.com`. **UEFI boot:** `efi\microsoft\boot\efisys.bin`
  (ou `efisys_noprompt.bin`, que não pede "Press any key"). O `make-iso.sh` já
  localiza os dois e monta ISO híbrida El Torito com xorriso — está correto.
- `ei.cfg` controla a edição/canal; `pid.txt` pode carregar chave. NTUnix não
  mexe (usuário traz licença) — ok.

### 1.2 install.wim vs install.esd — índices e edições

- **WIM** = arquivo append-only, cada arquivo interno deduplicado por hash,
  compressão **XPRESS** (rápida) ou **LZX** (padrão de install.wim). Um WIM tem
  **N imagens** (índices), uma por edição. `install.wim` da ISO oficial costuma
  ter Home, Home N, Home Single Language, Education, Pro, Pro N, Pro Education,
  Pro for Workstations, etc. (às vezes 6–11 índices).
- **ESD** = mesmo formato, mas compressão **LZMS "solid"** (recovery
  compression) — muito menor, porém **read-only na prática** e não montável para
  edição incremental fácil. As ISOs baixadas pela Media Creation Tool vêm com
  `install.esd`.
- **O `make-iso.sh` já normaliza `install.esd` → `install.wim`** exportando cada
  índice com `wimlib-imagex export ... --compress=LZX`. Correto. (Dá pra usar
  `--compress=LZMS --solid` no reempacote final para ISO menor, mas aí a imagem
  fica mais pesada de re-editar — ver §6/§1.6.)
- **`NTUNIX_EDITIONS`** permite tratar só um subconjunto de índices — bom, porque
  stripar 1 edição (ex.: Pro, índice típico 6) já basta e é muito mais rápido.

`ei.cfg`/`install.wim` guardam o **nome da edição** (`Name:`/`Edition ID:`), que
o `make-iso.sh` já lê via `wimlib-imagex info`.

### 1.3 boot.wim — índice 1 = WinPE, índice 2 = Setup

- `sources\boot.wim` tem **duas imagens**:
  - **Índice 1 — Microsoft Windows PE:** o WinPE "cru" (usado por recovery/PXE).
  - **Índice 2 — Microsoft Windows Setup:** WinPE **+ arquivos do Setup**; é o
    que roda quando você dá boot da mídia e vê a instalação.
- **É no boot.wim (WinPE) que o gate de requisitos roda** (SetupHost/appraiser),
  antes de qualquer coisa do `install.wim`. Por isso o bypass de LabConfig
  precisa existir **aqui** ou ser injetado no passo `windowsPE` do answer file
  (§5). tiny11 injeta os bypasses de LabConfig **nas duas** imagens (install +
  boot). O NTUnix hoje **não toca no boot.wim** — lacuna.

### 1.4 A pasta `sources`

Contém o motor do Setup: `setup.exe` → `SetupHost.exe` → (24H2) `setuphost.exe`
→ `SetupPrep.exe`. Também `appraiserres.dll`, `lang.ini`, `ei.cfg`, `sxs\`
(payloads de componentes que o Setup pode instalar). **Não mexer aqui** além do
que §1.6 pede (copiar `setuphost.exe`) — quebra a instalação.

### 1.5 `appraiserres.dll` — checagem de requisitos

- É a DLL do **compatibility appraiser** que o Setup carrega para avaliar
  TPM 2.0, Secure Boot, CPU (lista de modelos + instruções), RAM (4 GB) e disco.
- Truque histórico: **substituir por um arquivo de 0 byte** ou deletá-la faz o
  appraiser falhar "para cima" (não bloqueia). **Não é confiável no 24H2** — a
  Microsoft mudou o fluxo do Setup desde 22621; hoje o método robusto é
  **LabConfig no passo `windowsPE`** (§5), não zerar `appraiserres.dll`.
- Para **upgrade in-place** (não é o caso do NTUnix, que é clean install por
  `apply`), o que vale é `HKLM\SYSTEM\Setup\MoSetup /v
  AllowUpgradesWithUnsupportedTPMOrCPU = 1`.

### 1.6 Especificidades do 24H2 (build 26100/26200) — armadilhas reais

1. **`setuphost.exe` mismatch (quebra a instalação).** No 24H2 o novo Setup usa
   `setuphost.exe`. Se você regenera/edita a mídia (custom `install.wim`), o
   `setuphost.exe` de `\sources` pode divergir do que está dentro do
   `boot.wim`, e o Setup **aborta**. Fix documentado: **extrair `setuphost.exe`
   do `boot.wim` (índice 2, `\sources\setuphost.exe`) e copiá-lo para o
   `\sources\` da mídia** quando a build for ≥ 10.0.26100. **O NTUnix não faz
   isso hoje — adicionar ao `make-iso.sh`.**
2. **Bug de arquitetura do wimlib em imagens 24H2 capturadas.** wimlib (relatado
   em 1.14.4) determina a arquitetura lendo `Windows\System32\kernel32.dll`;
   em imagens 24H2 **capturadas de sistema sysprepado** ele pode marcar x64 como
   x86, e o Setup recusa ("cannot get image architecture", `setuperr.log`). Não
   ocorre com **mídia oficial intacta**. Conclusão para o NTUnix: **operar sobre
   o `install.wim` oficial (delete/add), nunca recapturar do zero.** O fluxo
   atual (export + update/delete/add) está do lado seguro.
3. **install.wim > 4 GB.** No 24H2 a imagem costuma passar de 4 GB; a ISO precisa
   de UDF (o `make-iso.sh` já passa `-udf` no xorriso — ok) e o pendrive de FAT32
   não aguenta o arquivo único (split com `wimlib-imagex split` em `.swm` se for
   gravar em FAT32; irrelevante se boot por UDF/exFAT/ISO em VM).
4. ISOs "\_v2" recentes (26200.x) podem vir **faltando componentes de identidade**
   (WAM/WinRE/dependências de Store) — outra razão para preferir a ISO oficial
   completa que o usuário fornece e validar `wimlib-imagex info` antes.

### 1.7 Inspecionar com wimlib (no Linux, sem Windows)

```bash
# lista TODAS as imagens/edições (Index, Name, Edition ID, tamanhos, arch)
wimlib-imagex info sources/install.wim

# detalhe de uma edição (ex.: índice 6 = Pro)
wimlib-imagex info sources/install.wim 6

# listar arquivos dentro de uma imagem (sem extrair)
wimlib-imagex dir sources/install.wim 6 --path='\Program Files\WindowsApps'
wimlib-imagex dir sources/install.wim 6 --path='\Windows\SystemApps'

# extrair um arquivo/pasta para inspeção
wimlib-imagex extract sources/install.wim 6 '\Windows\Web\Wallpaper' --dest-dir=/tmp/wp

# montar RW via FUSE (SÓ Linux) para editar arquivos e hives offline:
wimlib-imagex mountrw sources/install.wim 6 /mnt/wim
#   ... editar /mnt/wim ... (inclusive `reged`/`hivexregedit` nos hives)
wimlib-imagex unmount --commit /mnt/wim      # ou sem --commit para descartar

# apagar dentro da imagem (o que o make-iso.sh usa hoje)
wimlib-imagex update sources/install.wim 6 --command \
    "delete --force --recursive '\Program Files (x86)\Microsoft\Edge'"

# inspecionar o boot.wim
wimlib-imagex info sources/boot.wim          # 2 imagens: 1=WinPE, 2=Setup
```

Notas:
- `wimlib-imagex mountrw` é **FUSE, exclusivo de Linux/UNIX** — é o que permite
  editar **hives offline** (via `reged` do chntpw ou `hivexregedit`) sem Windows.
  No Windows equivalente seria `reg load`.
- `wimlib-imagex` **não** faz appx-servicing (não existe "wimlib
  remove-provisioned-appx"). Isso é a raiz da nuance de §3.

---

## 2. O que dá pra remover em nível de arquivo (offline) — e os caminhos

Tudo abaixo é aplicável **por edição** dentro do `install.wim` (e, quando dito,
também no `boot.wim`). Caminhos com `\` (formato interno WIM). Marcação:
**[raw ok]** = deletar arquivo é seguro; **[deprovisionar]** = NÃO deletar a
pasta, usar o fluxo de §3; **[registry]** = desativar, não apagar (§4).

### 2.1 OneDrive — [raw ok]
```text
\Windows\System32\OneDriveSetup.exe
\Windows\SysWOW64\OneDriveSetup.exe
```
É só o *instalador por-usuário*; deletar impede a instalação no primeiro logon.
Reforçar com `DisableFileSyncNGSC=1` (§4).

### 2.2 Edge integrado + updater — [raw ok], **manter WebView2**
```text
\Program Files (x86)\Microsoft\Edge
\Program Files (x86)\Microsoft\EdgeCore
\Program Files (x86)\Microsoft\EdgeUpdate
```
**NÃO** deletar `\Program Files (x86)\Microsoft\EdgeWebView` — é o WebView2
Runtime (Evergreen), do qual **Office, Teams, muitos instaladores/apps Win32
e até AutoCAD** dependem. O NTUnix já acerta nisso. Bônus: desligar o updater
com `EdgeUpdate` fora + tarefas `MicrosoftEdgeUpdateTask*` desabilitadas evita
reinstalação silenciosa do Edge.

### 2.3 Apps provisionados da Store — **[deprovisionar]**, ver §3
`\Program Files\WindowsApps` **não deve ser deletado inteiro por raw-delete**
(explicação e alternativa em §3). Isso cobre Copilot, Widgets (parte),
Teams (consumer), Xbox, Clipchamp, Bing*, Solitaire, People, ZuneMusic/Video,
YourPhone/CrossDevice, DevHome, PowerAutomate, QuickAssist, GetHelp, Getstarted,
Feedback Hub, Maps, Camera, SoundRecorder, StickyNotes, Todos, OutlookForWindows,
OneNote, OfficeHub, SkypeApp, Family, etc.

### 2.4 Copilot / Widgets / Teams — mistura de deprovisionar + registry
- **Copilot:** hoje é o pacote `Microsoft.Copilot` / `Microsoft.Windows.Copilot`
  (deprovisionar) **+** policy `...\WindowsCopilot /v TurnOffWindowsCopilot=1`.
- **Widgets:** `Microsoft.WindowsAppRuntime`-based `MicrosoftWindows.Client.WebExperience`
  (deprovisionar) + `...\Dsh /v AllowNewsAndInterests=0`; o "board" também some
  quando o shell vira `ntsession`. **Cuidado:** o Web Experience Pack tem
  dependências de runtime — deprovisionar (não raw-delete) evita órfãos.
- **Teams (consumer/"Chat"):** `MSTeams` / `MicrosoftTeams` (deprovisionar) +
  `...\Communications /v ConfigureChatAutoInstall=0` e `...\Chat /v ChatIcon=3`.

### 2.5 Wallpapers e mídia de tema extra — [raw ok]
```text
\Windows\Web\Wallpaper\ThemeA  ThemeB  ThemeC  ThemeD   (mantém \img0.jpg de \Wallpaper\Windows)
\Windows\Web\4K\Wallpaper\Windows    (versões 4K do img0 — grandes; opcional manter uma)
\Windows\Web\Screen                  (lockscreen art)
\Windows\Web\touchkeyboard
\Windows\Web\Wallpaper\Spotlight     (se existir)
```
Ganho de espaço pequeno mas seguro. Já no strip.list.

### 2.6 Drivers de terceiros (opcional, agressivo) — [raw ok com ressalva]
```text
\Windows\System32\DriverStore\FileRepository\   (INF packages de 3os)
```
**Ressalva forte:** o FileRepository também contém **drivers inbox que o PnP
pode precisar no primeiro boot** (armazenamento, chipset, rede). Deletar tudo é
o tipo de coisa que faz a máquina não subir por falta de driver de disco/rede.
tiny11 **não** mexe no DriverStore por isso. Recomendação NTUnix: **não incluir
no strip.list v1**; se for perseguir, remover **apenas** subpastas
reconhecidamente de impressora/scanner/`prn*.inf`, nunca em bloco.

### 2.7 .NET 3.5 / recursos opcionais, fontes de idioma extra — via componente
São **componentes**, não arquivos soltos: removê-los "na mão" corrompe o
component store. Só o `install.wim` "core" do tiny11 mexe nisso via
`dism /Remove-Package` — e isso **mata o servicing** (§6). Fora do escopo v1.

---

## 3. A forma CERTA de remover provisioned apps de imagem offline

### 3.1 Provisioned vs installed vs raw-delete — os três conceitos

- **Installed package (por-usuário):** já registrado num perfil. `Remove-AppxPackage`.
- **Provisioned package (por-imagem):** fica "engatilhado" para ser **instalado
  em todo novo perfil no próximo logon**. É o que interessa numa imagem: se você
  só apaga a pasta mas deixa o *provisioning*, o **AppX Deployment** ainda tenta
  registrar o pacote no primeiro logon → erros/lentidão, e pior, **o app pode
  voltar** num feature update.
- **Raw-delete de `\Program Files\WindowsApps`** (o que o NTUnix faz hoje):
  - remove **também os framework packages** compartilhados
    (`Microsoft.VCLibs*`, `Microsoft.NET.Native.*`, `Microsoft.UI.Xaml.*`,
    `Microsoft.WindowsAppRuntime.*`) dos quais outros pacotes dependem;
  - deixa o **provisioning órfão** (entradas em
    `SOFTWARE\...\Appx\AppxAllUserStore` apontando para arquivos que não existem);
  - **não cria** a chave `Deprovisioned`, então o app pode **ressurgir** no
    próximo feature update.

  Para o NTUnix, cujo shell é o `ntsession` (não o Explorer), o risco de "Start
  quebrado" é baixo, mas os **órfãos de provisioning** e a **perda de frameworks**
  são reais. É "funciona na maioria das vezes", não "correto".

### 3.2 O jeito canônico (Windows/DISM)

```powershell
# offline, imagem montada em C:\mount:
DISM /Image:C:\mount /Get-ProvisionedAppxPackages          # pega os PackageName
DISM /Image:C:\mount /Remove-ProvisionedAppxPackage /PackageName:Microsoft.BingWeather_4.25.20211.0_neutral_~_8wekyb3d8bbwe
DISM /Unmount-Image /MountDir:C:\mount /Commit
```
`/Remove-ProvisionedAppxPackage` **desregistra o provisioning** e, se o pacote
não estiver instalado em nenhum perfil, remove o pacote por completo. É o que o
`tiny11maker.ps1` faz — em loop sobre uma lista de **prefixos** de PackageName
(ver §7 para a lista real do tiny11).

### 3.3 O detalhe que morde: **offline não cria a chave `Deprovisioned`**

Documentação da Microsoft ("Keep removed apps from returning during an update"):
quando você desprovisiona um app, o Windows grava
```text
HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Appx\AppxAllUserStore\Deprovisioned\<PackageFamilyName>
```
que diz "não reinstale isto no próximo update". **Se a remoção é feita offline
(WIM montado), essa chave NÃO é criada** — e os apps voltam num feature update.
Solução da própria MS: **criar as chaves `Deprovisioned` manualmente** (um
subkey vazio por PackageFamilyName) antes de atualizar.

### 3.4 O problema específico do NTUnix: **build no Linux, sem DISM**

`make-iso.sh` roda em Linux com **wimlib**. DISM é binário Windows e **não roda
de forma confiável no Wine** (depende de CBS/serviços). Logo, o fluxo canônico
de §3.2 **não está disponível** no pipeline atual. Há três saídas:

- **(A) Recomendada — desprovisionar ONLINE no primeiro boot** (no
  `SetupComplete.cmd`, que roda como SYSTEM antes do primeiro logon):
  ```powershell
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "$keep='WindowsStore|WebView|VCLibs|NET.Native|UI.Xaml|WindowsAppRuntime|SecHealthUI|StorePurchaseApp|DesktopAppInstaller'; ^
     Get-AppxProvisionedPackage -Online | Where-Object { $_.DisplayName -notmatch $keep } | ^
     ForEach-Object { Remove-AppxProvisionedPackage -Online -PackageName $_.PackageName -ErrorAction SilentlyContinue }"
  ```
  Vantagens: remove arquivos **e** cria a chave `Deprovisioned` (persiste em
  update); **preserva frameworks** por allowlist; não depende de DISM no Linux;
  fácil de auditar/ajustar a whitelist. Custo: roda uma vez no primeiro boot
  (segundos) e o espaço só é liberado depois — aceitável.
- **(B) Offline via edição de hive (avançado, Linux-puro):** montar com
  `wimlib-imagex mountrw`, e com `reged`/`hivexregedit` editar o hive
  `\Windows\System32\config\SOFTWARE`: apagar as entradas de provisioning em
  `...\Appx\AppxAllUserStore\<PackageFullName>` **e** criar os subkeys em
  `...\Appx\AppxAllUserStore\Deprovisioned\<PackageFamilyName>`, e então
  deletar as pastas dos pacotes (menos frameworks). Mais trabalhoso e frágil;
  ganha espaço no WIM. Só vale se tamanho de ISO for crítico.
- **(C) Híbrido:** raw-delete só das **pastas de app puramente cosméticas**
  (ex.: jogos), mantendo frameworks, **+** a whitelist online de (A) para o
  resto. Melhor relação esforço/robustez se quiser algum ganho de espaço no WIM.

**Decisão sugerida:** adotar **(A)** já no v1 (poucas linhas no
`SetupComplete.cmd`), e **parar de raw-deletar `\Program Files\WindowsApps`
inteiro** no strip.list. Isso troca "funciona quase sempre" por "correto".

---

## 4. Desativar (não apagar): registry offline vs SetupComplete

### 4.1 Hive offline (`reg load` / `reged`/`hivexregedit`) vs primeiro boot

| Critério | Hive offline (na WIM) | SetupComplete / specialize (1º boot) |
|---|---|---|
| Onde roda | Linux, na imagem montada | Windows, como SYSTEM, pós-install |
| Ferramenta | `wimlib mountrw` + `reged`/`hivexregedit` | `reg add`, `sc`, `schtasks`, `powershell` |
| Robustez | frágil: nome do hive/chave muda por build; fácil corromper | alta: usa APIs vivas; idempotente; reversível |
| Serviços | dá pra setar `Start=4`, mas SCM pode reconfigurar no 1º boot | `sc config` definitivo, com o serviço "real" |
| Appx `Deprovisioned` | precisa criar as chaves na mão (§3.3) | criado automaticamente por `Remove-AppxProvisionedPackage -Online` |
| Auditabilidade | difícil (binário) | fácil (script texto, versionável) |
| Reversível pós-install | não (já está na imagem) | sim (`sc`, `schtasks /Enable`) |

**Conclusão (alinhada ao que o NTUnix já escolheu):** preferir **SetupComplete**
para serviços/telemetria/tarefas. Reservar edição de hive offline só para o que
**tem** que existir antes do primeiro boot — na prática, **o bypass de hardware
no `boot.wim`** (§5) e, se quiser espaço, o `Deprovisioned` de §3.4(B).

O `SetupComplete.cmd` roda **uma vez, como SYSTEM, ao fim do Setup e antes do
primeiro logon interativo** — é exatamente o momento certo para trocar o Shell
do Winlogon, desligar serviços e desprovisionar apps online.

### 4.2 O que é seguro desativar (reversível, não quebra logon/boot/servicing)

**Serviços** (`sc config <svc> start= disabled`), seguros:
- `DiagTrack` (Connected User Experiences and Telemetry) — o principal duto de telemetria.
- `dmwappushservice` (WAP Push / transporte de telemetria).
- `WSearch` (Windows Search/indexação) — NTUnix não usa; economiza CPU/IO.
- `SysMain` (Superfetch) — discutível em SSD; seguro desabilitar.
- `WerSvc` (Windows Error Reporting).
- `RetailDemo`, `MapsBroker`, `Fax`, `PcaSvc` (Program Compatibility Assistant),
  `WMPNetworkSvc`, `RemoteRegistry`, `lfsvc` (Geolocation), `DoSvc`/`DeliveryOptimization`
  (se aceitar WU mais lento), `WpnService`/`WpnUserService` (push notifications,
  se o shell próprio não usar).

**NÃO desabilitar** (quebra coisas): `wuauserv`, `UsoSvc`, `BITS`, `TrustedInstaller`
(CBS/servicing), `CryptSvc`, `RpcSs`, `DcomLaunch`, `Winmgmt` (WMI),
`WinDefend`/`SecurityHealthService`, `LSM`, `Power`, `PlugPlay`, `Schedule`,
`EventLog`, `nsi`, `Dhcp`, `Dnscache`, `mpssvc` (firewall), `ProfSvc`.

**Tarefas agendadas** (`schtasks /Change /TN "..." /Disable`), seguras:
```text
\Microsoft\Windows\Application Experience\Microsoft Compatibility Appraiser
\Microsoft\Windows\Application Experience\ProgramDataUpdater
\Microsoft\Windows\Application Experience\StartupAppTask
\Microsoft\Windows\Customer Experience Improvement Program\Consolidator
\Microsoft\Windows\Customer Experience Improvement Program\UsbCeip
\Microsoft\Windows\Windows Error Reporting\QueueReporting
\Microsoft\Windows\Feedback\Siuf\DmClient
\Microsoft\Windows\Feedback\Siuf\DmClientOnScenarioDownload
\Microsoft\Windows\DiskDiagnostic\Microsoft-Windows-DiskDiagnosticDataCollector
\Microsoft\Windows\Maps\MapsToastTask   +   \Maps\MapsUpdateTask
\Microsoft\Windows\Clip\License Validation   (se remover UWP)
```

**Políticas de registry** seguras (o tiny11 usa todas; HKLM = imagem, HKCU =
default user hive):
```text
SOFTWARE\Policies\Microsoft\Windows\DataCollection /v AllowTelemetry = 0
SOFTWARE\Microsoft\Windows\CurrentVersion\ContentDeliveryManager\* (SilentInstalledApps,
   PreInstalledApps, SubscribedContent-*, OemPreInstalled, SoftLanding = 0)
SOFTWARE\Policies\Microsoft\Windows\CloudContent /v DisableWindowsConsumerFeatures = 1
SOFTWARE\Policies\Microsoft\Windows\WindowsCopilot /v TurnOffWindowsCopilot = 1
SOFTWARE\Microsoft\Windows\CurrentVersion\AdvertisingInfo /v Enabled = 0   (HKCU)
SOFTWARE\Policies\Microsoft\Windows\OneDrive /v DisableFileSyncNGSC = 1
SOFTWARE\Microsoft\PolicyManager\current\device\Start /v ConfigureStartPins = "{}"
```
O `AllowTelemetry=0` só é "0 real" em edições Enterprise/Education; em Pro/Home o
mínimo efetivo é 1 (Required) — mas a policy + `DiagTrack` off já corta o grosso.

---

## 5. Bypass de requisitos do Win11 (24H2) — e o **bug atual do NTUnix**

### 5.1 O gate roda no `windowsPE`, não no `specialize`

O Setup avalia TPM/SecureBoot/RAM/CPU **no WinPE, no comecinho** (SetupHost +
appraiser), **antes** de aplicar a imagem e **muito antes** do passo
`specialize`. Portanto, o bypass tem que existir **antes** desse gate:

- **Caminho A (answer file):** injetar as chaves LabConfig no **passo
  `windowsPE`**, no componente **`Microsoft-Windows-Setup`**, via
  `RunSynchronous` — não no `Microsoft-Windows-Deployment/specialize`.
- **Caminho B (imagem):** gravar as chaves na **registry do `boot.wim` (WinPE)**
  em `HKLM\SYSTEM\Setup\LabConfig` (o que o tiny11 faz nas duas imagens).

### 5.2 O defeito no `build/autounattend.xml`

Hoje o NTUnix coloca:
```xml
<settings pass="specialize">
  <component name="Microsoft-Windows-Deployment" ...>
    <RunSynchronous> ... reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck ...
```
Isso roda **tarde demais**: num **clean install** em hardware sem TPM/SecureBoot,
o Setup **já barrou** no WinPE antes do `specialize`. Resultado: em máquina não
suportada, a instalação **não passa da tela de requisitos**. (Em VM/hardware que
já cumpre os requisitos, "funciona" — por isso o bug é fácil de não perceber.)

### 5.3 Correção recomendada (as chaves que funcionam no 24H2)

Adicionar um bloco `windowsPE` no `autounattend.xml`:
```xml
<settings pass="windowsPE">
  <component name="Microsoft-Windows-Setup" processorArchitecture="amd64"
             publicKeyToken="31bf3856ad364e35" language="neutral" versionScope="nonSxS"
             xmlns:wcm="http://schemas.microsoft.com/WMIConfig/2002/State">
    <RunSynchronous>
      <RunSynchronousCommand wcm:action="add"><Order>1</Order>
        <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassTPMCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>
      <RunSynchronousCommand wcm:action="add"><Order>2</Order>
        <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassSecureBootCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>
      <RunSynchronousCommand wcm:action="add"><Order>3</Order>
        <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassRAMCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>
      <RunSynchronousCommand wcm:action="add"><Order>4</Order>
        <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassCPUCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>
      <RunSynchronousCommand wcm:action="add"><Order>5</Order>
        <Path>reg add HKLM\SYSTEM\Setup\LabConfig /v BypassStorageCheck /t REG_DWORD /d 1 /f</Path></RunSynchronousCommand>
    </RunSynchronous>
  </component>
</settings>
```
Chaves aceitas no 24H2 (todas sob `HKLM\SYSTEM\Setup\LabConfig`, DWORD=1):
`BypassTPMCheck`, `BypassSecureBootCheck`, `BypassRAMCheck`, `BypassCPUCheck`,
`BypassStorageCheck`. Complementar (para upgrade, defensivo):
`HKLM\SYSTEM\Setup\MoSetup /v AllowUpgradesWithUnsupportedTPMOrCPU = 1`.

**Belt-and-suspenders (recomendado):** além do answer file, gravar as mesmas
chaves no **`boot.wim` índice 2** (via `wimlib mountrw` + `reged`), como o tiny11
faz — garante o bypass mesmo se o answer file não for lido a tempo. Manter
também o bloco `specialize` atual não faz mal, mas sozinho não resolve.

### 5.4 Limites do bypass
Nenhum bypass adiciona **instruções de CPU ausentes**. O 24H2 passou a **exigir
de fato SSE4.2 e POPCNT** no `ntoskrnl`; CPUs sem isso (pré-2008/2009) **não
bootam** o 24H2, com ou sem LabConfig. Documentar como limite de hardware.

---

## 6. Servicing / atualização — o que preserva a capacidade de atualizar

### 6.1 O que quebra o Windows Update quando você stripa demais
- Remover/mexer em **`WinSxS` (component store)** ou no **servicing stack** →
  CBS não consegue montar o estado dos componentes; qualquer cumulativa falha
  (0x800f08xx / 0x800f0831). SSU **não pode ser desinstalado** e precisa estar
  **≥** ao build; danificá-lo trava o próprio update dele (deadlock de repair).
- Remover **Defender offline** → corrompe o servicing (a Microsoft trata o
  pacote Defender como servicing component; a remoção "por arquivo" deixa o CBS
  inconsistente). O `tiny11coremaker.ps1` faz isso **de propósito** e por isso
  avisa que a imagem **não é serviceável**.
- Remover pacotes via `dism /Remove-Package` (IE, MediaPlayer, WordPad,
  Language FoD, Defender) → mesmo efeito: some a serviceabilidade.

### 6.2 Duas estratégias possíveis para o NTUnix
- **(I) Imagem serviceável (o que o v0 já é):** manter WinSxS, servicing stack,
  CBS/TrustedInstaller, Defender, `wuauserv`/`UsoSvc`/`BITS`. Só **desprovisionar
  apps** + **desativar serviços/telemetria** + **raw-delete do óbvio** (Edge,
  OneDrive, wallpapers). Windows Update continua funcionando; feature updates
  podem **repor** algum app se não houver a chave `Deprovisioned` (por isso §3.4).
  É a rota recomendada enquanto o NTUnix depende do WU da Microsoft para
  segurança do kernel/drivers.
- **(II) Imagem imutável estilo distro (VISAO.md §22 — A/B):** se o NTUnix for
  assumir seu **próprio** mecanismo de atualização (partições A/B, snapshots,
  imagens transacionais, rollback), aí **pode** stripar agressivo (WinSxS
  podado, Defender fora, `dism /Remove-Package`) como o tiny11 **core**, porque
  não se conta mais com o WU: cada update é uma **imagem nova aplicada em
  bloco**, com fallback para a partição anterior. Custo: o NTUnix vira o
  responsável por trazer patches de segurança do kernel/drivers NT — trabalho
  grande e contínuo.

**Recomendação:** ficar em **(I)** até o `packaged`/A-B de §22 existir de fato.
A agressividade extra (WinSxS/Defender/Remove-Package) **só** depois que o
NTUnix controlar o ciclo de atualização — senão a máquina fica sem patch de
segurança e sem conseguir se consertar.

---

## 7. NUNCA remover — lista com o porquê técnico

| Caminho / componente | Por que preservar |
|---|---|
| `\Windows\SystemApps\` (todo) | Contém **LogonUI**, `ShellExperienceHost`, `StartMenuExperienceHost`, `TextInputHost`, `SecHealthUI`, `Windows.CBSPreview`. LogonUI é a **tela de logon**; sem ela o Winlogon não completa o logon (mesmo com shell custom). Remoção offline = boot em tela preta/loop. |
| `\Windows\WinSxS\` (component store) | Fonte de verdade do CBS/servicing: hardlinks para todo binário do SO. Deletar/podar → **todo Windows Update falha** e `sfc`/`DISM /RestoreHealth` não conseguem reparar. |
| Servicing stack (`...\WinSxS\...servicingstack...`, `TiWorker`, `TrustedInstaller`) | SSU é permanente e precisa estar ≥ build; danificado → deadlock de update. |
| Windows Defender (`\ProgramData\Microsoft\Windows Defender`, pacote `Windows-Defender-Client-Package`) | Remoção offline **corrompe o servicing** (é componente CBS). Se não quiser Defender, **desligue** por policy/serviço, não apague. |
| WebView2 Runtime (`\Program Files (x86)\Microsoft\EdgeWebView`) | Dependência de **Office, Teams, muitos instaladores/apps Win32**; sem ele, apps que embutem WebView2 não abrem ("component missing"). |
| Framework packages em WindowsApps (`Microsoft.VCLibs*`, `Microsoft.NET.Native.*`, `Microsoft.UI.Xaml.*`, `Microsoft.WindowsAppRuntime.*`) | São **dependências compartilhadas** de outros pacotes; raw-delete em bloco os leva junto e quebra o AppX Deployment no 1º logon. |
| CBS/WMI/RPC core (`wuauserv`, `UsoSvc`, `BITS`, `TrustedInstaller`, `Winmgmt`, `RpcSs`, `DcomLaunch`, `CryptSvc`) | Base de servicing, ativação, PnP, cripto e IPC. Desligar = update/instalação/logon quebrados. |
| `boot.wim` core, `bootmgr`, `winload`, `\efi\microsoft\boot\*`, BCD | Sem eles a mídia não dá boot / o SO instalado não sobe. |
| `\sources\setup.exe`, `setuphost.exe`, `appraiserres.dll`, `\sources\sxs\` | Motor do Setup; mexer quebra a instalação (ver §1.6). |
| `smss/csrss/wininit/lsass/services/svchost` e `\Windows\System32\*` core | Processos estruturais de sessão/segurança que o VISAO.md §3 lista como preservados. |

---

## 8. `strip.list` revisada e priorizada

Legenda de risco: **[S]** seguro/comprovado · **[M]** moderado (validar em VM) ·
**[A]** agressivo (só com A/B de §6-II). Legenda de método: **raw** = fica no
`strip.list`; **boot** = editar também `boot.wim`; **online** = via
`SetupComplete.cmd` no 1º boot; **hive** = editar hive offline.

### Tier 1 — seguro, manter/entrar no strip.list (raw-delete) **[S]**
```text
# OneDrive (instalador por-usuário)
\Windows\System32\OneDriveSetup.exe
\Windows\SysWOW64\OneDriveSetup.exe

# Edge integrado + updater (MANTER EdgeWebView / WebView2!)
\Program Files (x86)\Microsoft\Edge
\Program Files (x86)\Microsoft\EdgeCore
\Program Files (x86)\Microsoft\EdgeUpdate

# Wallpapers/mídia de tema extra (mantém \Wallpaper\Windows\img0.jpg)
\Windows\Web\Wallpaper\ThemeA
\Windows\Web\Wallpaper\ThemeB
\Windows\Web\Wallpaper\ThemeC
\Windows\Web\Wallpaper\ThemeD
\Windows\Web\Wallpaper\Spotlight        # se existir
\Windows\Web\Screen
\Windows\Web\4K\Wallpaper\Windows        # 4K do img0 (grande) — opcional
\Windows\Web\touchkeyboard
```
Justificativa: todos são **arquivos soltos sem papel em logon/servicing**. Edge é
reinstalável/desabilitável; OneDrive é só o setup; wallpapers são cosméticos.

### Tier 2 — trocar raw-delete de WindowsApps por **desprovisionar online** **[S/M]**
- **Remover do strip.list:** `\Program Files\WindowsApps` (raw-delete inteiro).
- **Adicionar ao `SetupComplete.cmd`** o loop PowerShell de §3.4(A), com
  **allowlist** de frameworks + `WindowsStore` + `SecHealthUI` +
  `DesktopAppInstaller`/`StorePurchaseApp` (se quiser winget/Store).
- Alvos de desprovisionamento (prefixos de PackageName, base tiny11, ajustar):
```text
Clipchamp.Clipchamp  Microsoft.BingNews  Microsoft.BingWeather  Microsoft.BingSearch
Microsoft.Copilot  Microsoft.Windows.Copilot  Microsoft.Windows.DevHome
Microsoft.Windows.CrossDevice  Microsoft.GamingApp  Microsoft.GetHelp
Microsoft.Getstarted  Microsoft.MicrosoftOfficeHub  Microsoft.MicrosoftSolitaireCollection
Microsoft.MicrosoftStickyNotes  Microsoft.Paint  Microsoft.MSPaint  Microsoft.People
Microsoft.PowerAutomateDesktop  Microsoft.OutlookForWindows  Microsoft.Office.OneNote
Microsoft.OfficePushNotificationUtility  Microsoft.SkypeApp  Microsoft.Todos
Microsoft.WindowsAlarms  Microsoft.WindowsCamera  microsoft.windowscommunicationsapps
Microsoft.WindowsFeedbackHub  Microsoft.WindowsMaps  Microsoft.WindowsSoundRecorder
Microsoft.YourPhone  Microsoft.ZuneMusic  Microsoft.ZuneVideo  Microsoft.549981C3F5F10  (Cortana)
Microsoft.Xbox.TCUI  Microsoft.XboxGameOverlay  Microsoft.XboxGamingOverlay
Microsoft.XboxIdentityProvider  Microsoft.XboxSpeechToTextOverlay  Microsoft.GamingApp
MSTeams  MicrosoftTeams  MicrosoftCorporationII.QuickAssist  MicrosoftCorporationII.MicrosoftFamily
AppUp.IntelManagementandSecurityStatus  DolbyLaboratories.*  Microsoft.MixedReality.Portal
Microsoft.Wallet  Microsoft.StartExperiencesApp  Microsoft.WindowsTerminal(opcional)
```
Justificativa: desprovisionar (não raw-delete) preserva frameworks, evita
órfãos e **cria a chave `Deprovisioned`** → os apps não voltam em feature update.

### Tier 3 — bypass de hardware **[S]** (corrigir, ver §5)
- `autounattend.xml`: mover LabConfig para o passo **`windowsPE`** (mantendo o
  `specialize` como redundância). **boot** — gravar LabConfig também no
  `boot.wim` índice 2.

### Tier 4 — telemetria/serviços/tarefas via `SetupComplete.cmd` **[S]**
- Manter o que já existe e **expandir** com a lista de §4.2 (serviços seguros +
  tarefas + políticas de ContentDelivery/Copilot/Ads/OneDrive). Já é o lugar certo.

### Tier 5 — agressivo, **só com A/B de §6-II** **[A]** (NÃO no v1)
```text
# só em imagem imutável, que abre mão de Windows Update:
- poda de WinSxS (manter só common-controls, servicingstack, Catalogs, FileMaps,
  Fusion, Manifests) — como tiny11 CORE
- dism /Remove-Package: InternetExplorer-Optional, MediaPlayer, WordPad-FoD,
  Windows-Defender-Client, Language FoD (Handwriting/OCR/Speech/TextToSpeech)
- DriverStore\FileRepository (só impressora/scanner, nunca em bloco)
```
Justificativa: cada item aqui **mata o servicing**; só faz sentido quando o
NTUnix tiver o próprio update transacional.

### Fora do strip.list, no `make-iso.sh` (correções de build) **[S]**
- **24H2:** copiar `setuphost.exe` do `boot.wim` (índice 2, `\sources\`) para o
  `\sources\` da mídia quando build ≥ 26100 (§1.6.1).
- Operar sobre o `install.wim` oficial (delete/add); **não recapturar** (§1.6.2).

---

## 9. Diffs concretos sugeridos para o repo (resumo acionável)

1. **`build/autounattend.xml`** — adicionar bloco `pass="windowsPE"` /
   `Microsoft-Windows-Setup` com as 5 chaves LabConfig (§5.3). **[bug/alta prioridade]**
2. **`build/make-iso.sh`** —
   (a) após o `update`/`delete`/`add`, se `wimlib-imagex info boot.wim 2` disser
   build ≥ 26100, extrair `\sources\setuphost.exe` do `boot.wim` e copiá-lo para
   `$IMG/sources/`;
   (b) gravar LabConfig no `boot.wim` índice 2 (mountrw + reged) — redundância do bypass.
3. **`build/strip.list`** — remover a linha `\Program Files\WindowsApps`
   (raw-delete); manter Tier 1; comentar a mudança.
4. **`build/SetupComplete.cmd`** — adicionar o loop PowerShell de
   desprovisionamento com allowlist (§3.4A); expandir serviços/tarefas/políticas
   com a lista de §4.2.

---

## 10. Erros mais comuns que quebram a imagem (checklist de "não faça")

1. **Bypass no passo errado** (`specialize` em vez de `windowsPE`/`boot.wim`) →
   clean install barra na tela de requisitos em HW sem TPM.
2. **Raw-delete de `WindowsApps` inteiro** → frameworks perdidos + provisioning
   órfão + apps voltam no update (falta chave `Deprovisioned`).
3. **Esquecer o `setuphost.exe` no 24H2** → Setup aborta por mismatch de binário.
4. **Podar WinSxS / remover Defender offline** → servicing corrompido, WU nunca
   mais funciona (só aceitável em modelo A/B).
5. **Remover `SystemApps` (LogonUI)** → boot em tela preta / logon nunca completa.

---

## 11. Fontes (auditadas, não resenhas)

- tiny11builder (NTDEV), scripts reais:
  - https://github.com/ntdevlabs/tiny11builder
  - https://raw.githubusercontent.com/ntdevlabs/tiny11builder/main/tiny11maker.ps1 (regular — desprovisiona via `dism /Remove-ProvisionedAppxPackage`; Edge/OneDrive por raw-delete; LabConfig em install.wim **e** boot.wim)
  - https://raw.githubusercontent.com/ntdevlabs/tiny11builder/main/tiny11Coremaker.ps1 (core — poda WinSxS, remove Defender/IE/MediaPlayer via `dism /Remove-Package`; avisa que perde serviceabilidade)
  - fork 24H2/25H2: https://github.com/chrisGrando/tiny11maker-reforged
- Microsoft Learn:
  - DISM App Package servicing (offline `/Get-`/`/Remove-ProvisionedAppxPackage`): https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/dism-app-package--appx-or-appxbundle--servicing-command-line-options
  - "Keep removed apps from returning during an update" (chave `Deprovisioned`, offline não a cria): https://learn.microsoft.com/en-us/windows/application-management/remove-provisioned-apps-during-update
  - Remove-AppxProvisionedPackage (cmdlet): https://learn.microsoft.com/en-us/powershell/module/dism/remove-appxprovisionedpackage
  - Update Windows installation media with Dynamic Update (copiar setuphost.exe do boot.wim no 24H2): https://learn.microsoft.com/en-us/windows/deployment/update/media-dynamic-update
  - Win11 fails to install com custom install.wim (24H2): https://learn.microsoft.com/en-us/answers/questions/3847420/
  - WebView2 é parte do Win11 e dependência de Office/apps: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution ; https://learn.microsoft.com/en-us/microsoft-365-apps/deploy/webview2-install
  - Servicing stack não removível / component store corruption: https://learn.microsoft.com/en-us/answers/questions/5843473/ ; https://learn.microsoft.com/en-us/troubleshoot/windows-server/installing-updates-features-roles/fix-windows-update-errors
  - SystemApps/ShellExperienceHost/StartMenuExperienceHost essenciais: https://learn.microsoft.com/en-us/answers/questions/5792962/
- wimlib:
  - man wimlib-imagex (info/dir/update/delete/export/mountrw): https://wimlib.net/man1/wimlib-imagex.html ; https://man.archlinux.org/man/wimlib-imagex.1.en
  - bug de arquitetura em imagem 24H2 capturada: https://wimlib.net/forums/viewtopic.php?t=816
- Bypass 24H2 / passo windowsPE:
  - https://woshub.com/windows-11-unsupported-hardware-no-tpm-secure-boot/
  - https://www.elevenforum.com/t/w11-24h2-ltsc-cant-make-unattended-install-hardware-bypasses-and-copyprofile-work-together.36697/
  - https://www.elevenforum.com/t/finally-updating-24h2-media-with-all-the-latest-updates-is-working.29290/
- appraiserres.dll (histórico, patchado no fluxo novo):
  - https://www.elevenforum.com/t/installing-windows-11-by-removing-appraiserres-dll-file.3879/
