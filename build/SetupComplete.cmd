@echo off
rem ============================================================
rem NTUnix — SetupComplete.cmd
rem Executado pelo Windows Setup como SYSTEM, apos a instalacao e
rem antes do primeiro logon. E aqui que o user space padrao sai de
rem cena: shell da sessao vira o ntsession e servicos/telemetria
rem nao essenciais sao desligados (sem edicao offline de hive).
rem ============================================================
set ROOT=C:\NTUnix
set LOG=%ROOT%\var\log\setupcomplete.log

mkdir "%ROOT%\var\log" 2>nul
echo [%date% %time%] NTUnix SetupComplete iniciando > "%LOG%"

rem --- ambiente global -----------------------------------------
setx /M NTUNIX_ROOT "%ROOT%" >> "%LOG%" 2>&1

rem --- shell da sessao: ntsession no lugar do Explorer ---------
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" ^
    /v Shell /t REG_SZ /d "%ROOT%\system\bin\ntsession.exe" /f >> "%LOG%" 2>&1

rem --- OOBE sem conta Microsoft --------------------------------
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\OOBE" ^
    /v BypassNRO /t REG_DWORD /d 1 /f >> "%LOG%" 2>&1

rem --- servicos nao essenciais (conservador; reversivel com sc) -
for %%S in (DiagTrack dmwappushservice WSearch WerSvc SysMain
            RetailDemo MapsBroker Fax) do (
    sc config %%S start= disabled >> "%LOG%" 2>&1
)

rem --- tarefas agendadas de telemetria --------------------------
for %%T in (
    "\Microsoft\Windows\Application Experience\Microsoft Compatibility Appraiser"
    "\Microsoft\Windows\Customer Experience Improvement Program\Consolidator"
    "\Microsoft\Windows\Customer Experience Improvement Program\UsbCeip"
    "\Microsoft\Windows\Windows Error Reporting\QueueReporting"
) do (
    schtasks /Change /TN %%T /Disable >> "%LOG%" 2>&1
)

rem --- telemetria no minimo permitido ---------------------------
reg add "HKLM\SOFTWARE\Policies\Microsoft\Windows\DataCollection" ^
    /v AllowTelemetry /t REG_DWORD /d 0 /f >> "%LOG%" 2>&1

echo [%date% %time%] NTUnix SetupComplete concluido >> "%LOG%"
exit /b 0
