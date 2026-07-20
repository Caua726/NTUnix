@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ============================================================================
rem ntstrap - o instalador do NTUnix. Roda DENTRO do ambiente live (WinPE).
rem
rem   ntstrap [disco] [perfil]      ex.: ntstrap 0 leve
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
set INDEX=%~2
if "%DISK%"=="" set DISK=0
set EFI=S:
set TGT=W:
rem A arvore do NTUnix vem do proprio ambiente live: ela esta dentro do
rem boot.wim, montado em X:. So o install.wim e' grande demais e fica na midia.
set NTU=%SYSTEMDRIVE%\NTUnix
set INST=%NTU%\install

echo.
echo   ntstrap - instalador do NTUnix
echo   ------------------------------

rem --- 1. achar a midia (a letra do CD varia; procuramos o install.wim) ------
set SRC=
for %%D in (D E F G H I J K L M N O P Q R T U V Y Z) do (
    if exist "%%D:\sources\install.wim" if not defined SRC set SRC=%%D:
)
if not defined SRC (
    echo   erro: nao achei \sources\install.wim em nenhuma unidade.
    echo         a midia live precisa ser gerada com NTUNIX_INSTALLER=1.
    exit /b 1
)
rem Base do sistema. Hoje o install.wim, ja enxugado no host pelo make-live.
rem NTU_BASE permite apontar outra: o boot.wim (WinPE persistente) e o proximo
rem perfil, bem menor, e o ambiente live ja prova que ele roda o desktop.
if "%NTU_BASE%"=="" set NTU_BASE=%SRC%\sources\install.wim
set BASE=%NTU_BASE%
echo   base:    %BASE%

rem --- 2. escolher o PERFIL -------------------------------------------------
rem A midia traz os tres como imagens do mesmo wim. Tamanhos medidos sobre um
rem Windows 11 Pro; ver docs/perfis.md.
if /I "%INDEX%"=="normal" set INDEX=1
if /I "%INDEX%"=="leve"   set INDEX=2
if /I "%INDEX%"=="debug"  set INDEX=3
if "%INDEX%"=="" (
    echo.
    echo     1  normal   ~4.8 GB   .NET, 32 bits e drivers completos
    echo     2  leve     ~3.0 GB   sem .NET nem 32 bits
    echo     3  debug    ~670 MB   SO PARA VM: base minima + navegador
    echo.
    set /p INDEX=  perfil ^(1/2/3 ou normal/leve/debug^):
)
if /I "%INDEX%"=="normal" set INDEX=1
if /I "%INDEX%"=="leve"   set INDEX=2
if /I "%INDEX%"=="debug"  set INDEX=3
if "%INDEX%"=="" (
    echo   erro: nenhum perfil escolhido.
    exit /b 1
)

echo   disco:   %DISK%   ^(SERA APAGADO POR COMPLETO^)
echo   perfil:  imagem %INDEX%
echo.
set /p OK=  digite SIM para continuar:
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
rem DEV: a arvore vem do share SMB do host, ao vivo. As duas travas precisam
rem cair porque o smbd do QEMU serve com 'guest ok=yes' e sem assinatura.
if exist "%NTU%\etc\ntunix-debug" (
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
if exist "%INST%\unattend-oobe.xml" (
    echo   ==^> plantando o unattend de primeiro boot ^(so conta + autologon^)
    if not exist "%TGT%\Windows\Panther" mkdir "%TGT%\Windows\Panther"
    copy /y "%INST%\unattend-oobe.xml" "%TGT%\Windows\Panther\unattend.xml" >nul
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
