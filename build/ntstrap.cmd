@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ============================================================================
rem ntstrap - o instalador do NTUnix. Roda DENTRO do ambiente live.
rem
rem   ntstrap                 menu guiado (padrao)
rem   ntstrap <disco> <perfil>  direto, sem menu:  ntstrap 0 leve
rem
rem Faz o que o pacstrap faz: particiona, aplica o sistema base num diretorio e
rem torna aquilo bootavel. O Setup da Microsoft NAO participa - por isso nao ha
rem tela de idioma, nem chave de produto, nem OOBE decidindo nada por nos. A
rem configuracao e' escrita direto nos hives, offline, antes do primeiro boot.
rem
rem Ferramentas: diskpart, dism, bcdboot e reg sao todas inbox no WinPE.
rem ============================================================================

rem PATH proprio. Quando o ntstrap e' chamado do shell do NTUnix, o cmd.exe
rem herda um PATH estilo Unix (/mnt/x/windows/system32) que ele nao entende -
rem e perde diskpart, dism, reg e bcdboot de uma vez so'.
set PATH=%SystemRoot%\System32;%SystemRoot%;%SystemRoot%\System32\Wbem

set DISK=%~1
set PERFIL=%~2
rem defaults; o menu sobrescreve. HOSTNAME vazio herdaria o nome aleatorio do
rem WinPE (MININT-XXXXXXX), que e' o que acontecia antes.
set HOSTNAME=ntunix
set USUARIO=ntunix
set SENHA=ntunix
set KBD=0416:00000416
set KBDNOME=pt-BR (ABNT2)
set FUSO=E. South America Standard Time
set DEVMODE=nao
set EFI=S:
set TGT=W:
rem A arvore do NTUnix vem do proprio ambiente live: ela esta dentro do
rem boot.wim, montado em X:. So o install.wim e' grande demais e fica na midia.
set NTU=%SYSTEMDRIVE%\NTUnix
set INST=%NTU%\install

rem --- 1. achar a midia (a letra do CD varia; procuramos o install.wim) ------
set SRC=
for %%D in (D E F G H I J K L M N O P Q R T U V Y Z) do (
    if exist "%%D:\sources\install.wim" if not defined SRC set SRC=%%D:
)
if not defined SRC (
    echo   erro: nao achei \sources\install.wim em nenhuma unidade.
    exit /b 1
)
if "%NTU_BASE%"=="" set NTU_BASE=%SRC%\sources\install.wim
set BASE=%NTU_BASE%

rem se vieram os dois argumentos, pula o menu
if not "%DISK%"=="" if not "%PERFIL%"=="" goto :resolve
if "%DISK%"=="" set DISK=0
if "%PERFIL%"=="" set PERFIL=leve

rem ============================ MENU GUIADO ==================================
:menu
cls
echo.
echo    ntstrap - instalador do NTUnix
echo    ==============================
echo.
echo      midia:  %BASE%
echo.
echo      1^)  Disco de destino .... %DISK%
echo      2^)  Perfil ............... %PERFIL%
echo      3^)  Hostname ............. %HOSTNAME%
echo      4^)  Usuario .............. %USUARIO%
echo      5^)  Senha ................ ^(definida^)
echo      6^)  Teclado .............. %KBDNOME%
echo      7^)  Fuso horario ......... %FUSO%
echo      8^)  Arvore no share SMB .. %DEVMODE%
echo.
echo      i^)  Instalar
echo      q^)  Sair
echo.
set OPC=
set /p OPC=   escolha: 
if /I "%OPC%"=="1" goto :escolhe_disco
if /I "%OPC%"=="2" goto :escolhe_perfil
if /I "%OPC%"=="3" goto :escolhe_host
if /I "%OPC%"=="4" goto :escolhe_user
if /I "%OPC%"=="5" goto :escolhe_senha
if /I "%OPC%"=="6" goto :escolhe_kbd
if /I "%OPC%"=="7" goto :escolhe_fuso
if /I "%OPC%"=="8" goto :alterna_dev
if /I "%OPC%"=="i" goto :resolve
if /I "%OPC%"=="q" exit /b 1
goto :menu

:escolhe_disco
cls
echo.
echo    Discos disponiveis
echo    ------------------
echo list disk > "%TEMP%\ld.txt"
diskpart /s "%TEMP%\ld.txt" | findstr /R /C:"Disco" /C:"Disk"
echo.
set /p DISK=   numero do disco [%DISK%]: 
if "%DISK%"=="" set DISK=0
goto :menu

:escolhe_perfil
cls
echo.
echo    Perfil de instalacao
echo    --------------------
echo.
echo      1^)  normal    ~4.8 GB    .NET, 32 bits e drivers completos.
echo                                Compatibilidade maxima com apps Win32.
echo.
echo      2^)  leve      ~3.0 GB    Sem .NET nem 32 bits. Ainda instala em
echo                                hardware real; navegadores funcionam.
echo.
echo      3^)  debug     ~670 MB    SO PARA MAQUINA VIRTUAL. Base minima:
echo                                nosso user space + navegador. Drivers
echo                                so' de VM - nao boota em metal.
echo.
set OPC=
set /p OPC=   perfil [1/2/3]: 
if "%OPC%"=="1" set PERFIL=normal
if "%OPC%"=="2" set PERFIL=leve
if "%OPC%"=="3" set PERFIL=debug
goto :menu


:escolhe_host
echo.
set /p HOSTNAME=   hostname [%HOSTNAME%]: 
if "%HOSTNAME%"=="" set HOSTNAME=ntunix
goto :menu

:escolhe_user
echo.
set /p USUARIO=   usuario [%USUARIO%]: 
if "%USUARIO%"=="" set USUARIO=ntunix
goto :menu

:escolhe_senha
echo.
rem sem mascara: o WinPE nao da' leitura silenciosa em batch. A senha vai em
rem texto claro no unattend do primeiro boot de qualquer forma, e o proprio
rem Windows a apaga de la' apos consumir.
set /p SENHA=   senha para %USUARIO%: 
if "%SENHA%"=="" set SENHA=ntunix
goto :menu

:escolhe_kbd
cls
echo.
echo    Layout de teclado
echo    -----------------
echo      1^)  pt-BR (ABNT2)      2^)  pt-BR (ABNT)
echo      3^)  en-US              4^)  en-US Intl
echo      5^)  es-ES              6^)  de-DE
echo.
set OPC=
set /p OPC=   escolha: 
if "%OPC%"=="1" set KBD=0416:00000416& set KBDNOME=pt-BR (ABNT2)
if "%OPC%"=="2" set KBD=0416:00010416& set KBDNOME=pt-BR (ABNT)
if "%OPC%"=="3" set KBD=0409:00000409& set KBDNOME=en-US
if "%OPC%"=="4" set KBD=0409:00020409& set KBDNOME=en-US Intl
if "%OPC%"=="5" set KBD=0c0a:0000040a& set KBDNOME=es-ES
if "%OPC%"=="6" set KBD=0407:00000407& set KBDNOME=de-DE
goto :menu

:escolhe_fuso
cls
echo.
echo    Fuso horario
echo    ------------
echo      1^)  Brasilia        2^)  UTC
echo      3^)  Lisboa          4^)  Nova York
echo.
set OPC=
set /p OPC=   escolha: 
if "%OPC%"=="1" set FUSO=E. South America Standard Time
if "%OPC%"=="2" set FUSO=UTC
if "%OPC%"=="3" set FUSO=GMT Standard Time
if "%OPC%"=="4" set FUSO=Eastern Standard Time
goto :menu

:alterna_dev
rem DEV: a raiz aponta pro share SMB do host (\\10.0.2.4\qemu) em vez de
rem C:\NTUnix. Recompilar no host passa a valer no proximo restart do servico.
if /I "%DEVMODE%"=="nao" (set DEVMODE=sim) else (set DEVMODE=nao)
goto :menu

rem ====================== resolucao e confirmacao ============================
:resolve
set INDEX=
if /I "%PERFIL%"=="normal" set INDEX=1
if /I "%PERFIL%"=="leve"   set INDEX=2
if /I "%PERFIL%"=="debug"  set INDEX=3
if /I "%PERFIL%"=="1" set INDEX=1& set PERFIL=normal
if /I "%PERFIL%"=="2" set INDEX=2& set PERFIL=leve
if /I "%PERFIL%"=="3" set INDEX=3& set PERFIL=debug
if "%INDEX%"=="" (
    echo   erro: perfil desconhecido '%PERFIL%' ^(use normal, leve ou debug^).
    exit /b 1
)

cls
echo.
echo    Resumo
echo    ------
echo.
echo      disco    %DISK%      SERA APAGADO POR COMPLETO
echo      perfil   %PERFIL% ^(imagem %INDEX%^)
echo      hostname %HOSTNAME%
echo      usuario  %USUARIO% ^(Administrators, autologon^)
echo      teclado  %KBDNOME%
echo      fuso     %FUSO%
echo      raiz     %DEVMODE%
echo.
echo      Layout: ESP 300MB FAT32 + MSR 16MB + NTFS no resto
echo      O shell da sessao sera o ntsession, escrito no hive antes do 1o boot.
echo.
set OK=
set /p OK=   digite SIM para instalar: 
if /I not "%OK%"=="SIM" (
    echo   cancelado.
    exit /b 1
)

rem --- 3. particionar (GPT/UEFI) --------------------------------------------
rem Layout da MS: ESP 300MB FAT32 + MSR 16MB + Windows NTFS no resto.
echo.
echo   ==^> particionando o disco %DISK%
(
    echo select disk %DISK%
    echo clean
    echo convert gpt
    echo create partition efi size=300
    echo format quick fs=fat32 label="System"
    echo assign letter=%EFI:~0,1%
    echo create partition msr size=16
    echo create partition primary
    echo format quick fs=ntfs label="NTUnix"
    echo assign letter=%TGT:~0,1%
) > "%TEMP%\ntstrap-disk.txt"
diskpart /s "%TEMP%\ntstrap-disk.txt" || (
    echo   erro: diskpart falhou.
    exit /b 1
)

rem --- 4. aplicar o sistema base (o "pacstrap") -----------------------------
echo.
echo   ==^> aplicando a imagem em %TGT% ^(demora^)
rem /ScratchDir no destino: sem isso o dism usa o ramdisk do WinPE como area
rem temporaria e estoura ("nao ha recursos de memoria suficientes", erro 8).
if not exist "%TGT%\ntu-scratch" mkdir "%TGT%\ntu-scratch"
dism /Apply-Image /ImageFile:"%BASE%" /Index:%INDEX% /ApplyDir:%TGT%\ /ScratchDir:%TGT%\ntu-scratch || (
    echo   erro: dism /Apply-Image falhou.
    exit /b 1
)

rem --- 5. remover o user space padrao do Windows ----------------------------
rem O mesmo papel do strip.list, mas depois de aplicar: mais simples e mais
rem rapido do que editar o wim, e o resultado e' identico.
echo.
echo   ==^> removendo o user space padrao
if exist "%INST%\strip.list" (
    for /f "usebackq eol=# tokens=* delims=" %%P in ("%INST%\strip.list") do (
        if not "%%P"=="" (
            if exist "%TGT%%%P" rd /s /q "%TGT%%%P" 2>nul
            if exist "%TGT%%%P" del /f /q "%TGT%%%P" 2>nul
        )
    )
)

rem --- 6. instalar a arvore do NTUnix ---------------------------------------
echo   ==^> instalando \NTUnix
xcopy "%NTU%" "%TGT%\NTUnix\" /E /I /H /K /Y /EXCLUDE:%INST%\xcopy-skip.txt >nul || (
    echo   erro: copia da arvore do NTUnix falhou.
    exit /b 1
)

rem --- 7. configurar OFFLINE, nos hives do sistema aplicado -----------------
rem E' aqui que o sistema deixa de ser um Windows comum. Sem SetupComplete.cmd,
rem sem primeiro logon: quando esta maquina bootar pela primeira vez, o shell
rem da sessao ja e' o nosso.
echo   ==^> configurando o sistema ^(registro offline^)

reg load HKLM\NTUSOFT "%TGT%\Windows\System32\config\SOFTWARE" >nul || (
    echo   erro: nao consegui carregar o hive SOFTWARE.
    exit /b 1
)
rem shell da sessao: ntsession no lugar do explorer
reg add "HKLM\NTUSOFT\Microsoft\Windows NT\CurrentVersion\Winlogon" ^
    /v Shell /t REG_SZ /d "C:\NTUnix\system\bin\ntsession.exe" /f >nul
rem telemetria no minimo permitido
reg add "HKLM\NTUSOFT\Policies\Microsoft\Windows\DataCollection" ^
    /v AllowTelemetry /t REG_DWORD /d 0 /f >nul
rem OOBE sem conta Microsoft
reg add "HKLM\NTUSOFT\Microsoft\Windows\CurrentVersion\OOBE" ^
    /v BypassNRO /t REG_DWORD /d 1 /f >nul
reg unload HKLM\NTUSOFT >nul

reg load HKLM\NTUSYS "%TGT%\Windows\System32\config\SYSTEM" >nul || (
    echo   erro: nao consegui carregar o hive SYSTEM.
    exit /b 1
)
set ENV=HKLM\NTUSYS\ControlSet001\Control\Session Manager\Environment
reg add "%ENV%" /v NTUNIX_ROOT /t REG_SZ /d "C:\NTUnix" /f >nul
rem servicos nao essenciais (Start=4 e' disabled; reversivel com sc)
for %%S in (DiagTrack dmwappushservice WSearch WerSvc SysMain RetailDemo MapsBroker Fax) do (
    reg add "HKLM\NTUSYS\ControlSet001\Services\%%S" /v Start /t REG_DWORD /d 4 /f >nul 2>&1
)
rem nome da maquina: sem isto fica o aleatorio do WinPE (MININT-XXXXXXX)
reg add "HKLM\NTUSYS\ControlSet001\Control\ComputerName\ComputerName" ^
    /v ComputerName /t REG_SZ /d "%HOSTNAME%" /f >nul
reg add "HKLM\NTUSYS\ControlSet001\Services\Tcpip\Parameters" ^
    /v Hostname /t REG_SZ /d "%HOSTNAME%" /f >nul
reg add "HKLM\NTUSYS\ControlSet001\Services\Tcpip\Parameters" ^
    /v "NV Hostname" /t REG_SZ /d "%HOSTNAME%" /f >nul
rem fuso horario
reg add "HKLM\NTUSYS\ControlSet001\Control\TimeZoneInformation" ^
    /v TimeZoneKeyName /t REG_SZ /d "%FUSO%" /f >nul

rem DEV: a arvore vem do share SMB do host, ao vivo. As duas travas precisam
rem cair porque o smbd do QEMU serve com 'guest ok=yes' e sem assinatura.
if /I "%DEVMODE%"=="sim" (
    echo       [dev] raiz apontada pro share do host
    reg add "HKLM\NTUSYS\ControlSet001\Services\LanmanWorkstation\Parameters" ^
        /v AllowInsecureGuestAuth /t REG_DWORD /d 1 /f >nul
    reg add "HKLM\NTUSYS\ControlSet001\Services\LanmanWorkstation\Parameters" ^
        /v RequireSecuritySignature /t REG_DWORD /d 0 /f >nul
    reg add "%ENV%" /v NTUNIX_ROOT /t REG_SZ /d "\\10.0.2.4\qemu" /f >nul
)
reg unload HKLM\NTUSYS >nul

rem --- 8. conta local + autologon (unica coisa que o OOBE ainda faz) --------
rem Criar conta offline nao e' viavel; entao plantamos um unattend MINIMO que
rem so trata o pass oobeSystem. Nada de disco, idioma ou chave - aquilo era o
rem instalador da Microsoft, e ele nao roda mais.
rem O template vem com @PLACEHOLDERS@ e e' preenchido AQUI, nao no build: antes
rem a senha ia renderizada dentro da ISO, igual para todo mundo que usasse
rem aquela midia. Agora e' a que o instalador escolheu.
if exist "%INST%\unattend-oobe.xml" (
    echo   ==^> gerando o unattend de primeiro boot
    if not exist "%TGT%\Windows\Panther" mkdir "%TGT%\Windows\Panther"
    if exist "%TGT%\Windows\Panther\unattend.xml" del /f /q "%TGT%\Windows\Panther\unattend.xml"
    for /f "usebackq delims=" %%L in ("%INST%\unattend-oobe.xml") do (
        set "linha=%%L"
        set "linha=!linha:@NTUNIX_PW@=%SENHA%!"
        set "linha=!linha:@NTUNIX_USER@=%USUARIO%!"
        set "linha=!linha:@NTUNIX_HOST@=%HOSTNAME%!"
        set "linha=!linha:@NTUNIX_KBD@=%KBD%!"
        echo(!linha!>> "%TGT%\Windows\Panther\unattend.xml"
    )
)

rem --- 9. tornar bootavel ---------------------------------------------------
if exist "%TGT%\ntu-scratch" rd /s /q "%TGT%\ntu-scratch" 2>nul
echo   ==^> instalando o bootloader ^(UEFI^)
bcdboot %TGT%\Windows /s %EFI% /f UEFI || (
    echo   erro: bcdboot falhou.
    exit /b 1
)

echo.
echo   pronto. NTUnix instalado no disco %DISK%.
echo   remova a midia e reinicie:   wpeutil reboot
echo.
endlocal
exit /b 0
