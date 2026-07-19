/*
 * ntdbgcon.c — console de debug via porta serial (SO EM BUILD DE DEV).
 *
 * Abre a COM1 (que o QEMU liga a um socket no host), configura raw 115200 8N1
 * com leitura que bloqueia ate >=1 byte, e roda o busybox `sh` com stdin/stdout/
 * stderr = COM1. Assim o desenvolvedor (ou o assistente) conecta no socket serial
 * do host e cai num shell da NTUnix — sem depender da rede da VM, sem print/loop.
 *
 * A musl-nt reconhece um handle serial (GetCommState) como NT_FD_FILE, entao o
 * busybox le/escreve na serial por ReadFile/WriteFile (nao pelo caminho de
 * console). Re-spawna o shell se ele sair.
 *
 * Loga cada passo em <dir_do_exe>\ntdbgcon.log (leia no desktop:
 * `cat /system/bin/ntdbgcon.log`) pra diagnosticar quando o canal nao sobe.
 *
 * NAO deve entrar em build de producao (canal sem autenticacao). Gate no unit.
 */
#include <windows.h>

static char g_logpath[MAX_PATH];
static char g_dir[MAX_PATH];        /* diretorio do exe (= /system/bin) */

static void logln(const char *msg, unsigned long code)
{
    HANDLE h;
    char line[256];
    int n = 0;
    const char *p;
    DWORD w;
    for (p = msg; *p && n < 200; p++) line[n++] = *p;
    if (code != 0xFFFFFFFFu) {
        int i;
        line[n++] = ' '; line[n++] = '('; line[n++] = 'e'; line[n++] = '=';
        for (i = 28; i >= 0; i -= 4) line[n++] = "0123456789abcdef"[(code >> i) & 0xf];
        line[n++] = ')';
    }
    line[n++] = '\r'; line[n++] = '\n';
    h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, line, (DWORD)n, &w, NULL); CloseHandle(h); }
}

/* roda um comando, espera, e LOGA o exit code (review: nao mascarar o erro real). */
static void run_and_log(const char *exe, char *cmd, const char *label)
{
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    DWORD ec = 0xFFFFFFFEu;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    logln(label, 0xFFFFFFFFu);
    logln(cmd, 0xFFFFFFFFu);
    if (CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 25000);
        GetExitCodeProcess(pi.hProcess, &ec);
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        logln("  -> exit code", ec);
    } else {
        logln("  -> NAO rodou (GetLastError)", GetLastError());
    }
}

int main(void)
{
    HANDLE com;
    DCB dcb;
    COMMTIMEOUTS to;
    char *slash, *p;
    DWORD nmod;
    int tries;

    /* dir do exe + caminho do log + caminho do busybox */
    nmod = GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    if (nmod == 0 || nmod >= sizeof g_dir) return 1;
    slash = g_dir;
    for (p = g_dir; *p; p++) if (*p == '\\' || *p == '/') slash = p;
    *slash = 0;                                   /* g_dir = diretorio do exe */
    lstrcpyA(g_logpath, g_dir);
    lstrcatA(g_logpath, "\\ntdbgcon.log");

    logln("ntdbgcon: iniciando", 0xFFFFFFFFu);
    logln("ntdbgcon: dir", 0xFFFFFFFFu);
    logln(g_dir, 0xFFFFFFFFu);

    /* A serial ISA nao enumera no WinPE UEFI (sem NTDETECT.COM). Usamos serial PCI
     * (QEMU pci-serial 1b36:0002), que o WinPE enumera via PCI de forma confiavel.
     * Falta so o INF que identifica o hardware (qemupciserial.inf, embarcado ao
     * lado deste exe) — drvload dele faz o PnP bindar o serial.sys no filho
     * *PNP0501 -> a COM aparece. */
    {
        char win32[MAX_PATH], exe[MAX_PATH], inf[MAX_PATH], cmd[MAX_PATH * 2 + 16];
        GetSystemDirectoryA(win32, sizeof win32);   /* X:\Windows\System32 */
        /* 1) instala o INF da serial PCI (mapeia 1b36:0002 -> mf.inf -> serial.sys) */
        lstrcpyA(exe, win32); lstrcatA(exe, "\\drvload.exe");
        lstrcpyA(inf, g_dir); lstrcatA(inf, "\\qemupciserial.inf");
        cmd[0] = '"'; lstrcpyA(cmd + 1, exe);
        lstrcatA(cmd, "\" \""); lstrcatA(cmd, inf); lstrcatA(cmd, "\"");
        run_and_log(exe, cmd, "ntdbgcon: drvload qemupciserial.inf");
        /* 2) forca o PnP a re-escanear -> binda o INF no device VEN_1B36 e cria os
         * filhos COM (*PNP0501 -> serial.sys). Sem o rescan o WinPE nao enumera o
         * device recem-instalado (drvload so poe o driver no store). */
        lstrcpyA(exe, win32); lstrcatA(exe, "\\pnputil.exe");
        cmd[0] = '"'; lstrcpyA(cmd + 1, exe); lstrcatA(cmd, "\" /scan-devices");
        run_and_log(exe, cmd, "ntdbgcon: pnputil /scan-devices");
    }

    /* abre a serial: varre COM1..COM8 (pode enumerar como outro numero sob UEFI),
     * com retry (o driver serial pode demorar a subir no boot). */
    com = INVALID_HANDLE_VALUE;
    for (tries = 0; tries < 30 && com == INVALID_HANDLE_VALUE; tries++) {
        char dev[] = "\\\\.\\COM1";
        int cn;
        for (cn = 1; cn <= 8; cn++) {
            dev[7] = (char)('0' + cn);
            com = CreateFileA(dev, GENERIC_READ | GENERIC_WRITE,
                              0, NULL, OPEN_EXISTING, 0, NULL);
            if (com != INVALID_HANDLE_VALUE) { logln("ntdbgcon: serial aberta em COM", (unsigned)cn); break; }
            if (tries == 0) logln(dev, GetLastError());  /* review: erro de CADA COM (2=nao existe, 5=em uso) */
        }
        if (com == INVALID_HANDLE_VALUE) {
            if (tries == 0) logln("ntdbgcon: nenhuma COM1-8; retrying 30x...", 0xFFFFFFFFu);
            Sleep(1000);
        }
    }
    if (com == INVALID_HANDLE_VALUE) {
        logln("ntdbgcon: DESISTINDO — nenhuma serial enumerada no guest", 0xFFFFFFFFu);
        return 1;
    }

    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(com, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        /* raw COMPLETO: sem paridade, sem flow-control (senao WriteFile pode
         * bloquear se CTS/DSR estiver baixo), sem XON/XOFF, sem abort-on-error */
        dcb.fBinary  = TRUE;
        dcb.fParity  = FALSE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        dcb.fOutX = FALSE;
        dcb.fInX  = FALSE;
        dcb.fErrorChar = FALSE;
        dcb.fNull = FALSE;
        dcb.fAbortOnError = FALSE;
        dcb.fDtrControl = DTR_CONTROL_ENABLE;
        dcb.fRtsControl = RTS_CONTROL_ENABLE;
        if (!SetCommState(com, &dcb))
            logln("ntdbgcon: SetCommState falhou", GetLastError());
    } else {
        logln("ntdbgcon: GetCommState falhou", GetLastError());
    }
    ZeroMemory(&to, sizeof to);
    to.ReadIntervalTimeout        = MAXDWORD;       /* bloqueia ate >=1 byte */
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant   = MAXDWORD - 1;
    SetCommTimeouts(com, &to);
    SetHandleInformation(com, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    for (;;) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[MAX_PATH + 16], busybox[MAX_PATH];
        const char *banner = "\r\n[ntdbgcon] shell serial pronto — tecle enter\r\n";
        DWORD w;

        lstrcpyA(busybox, g_dir);
        lstrcatA(busybox, "\\busybox.exe");

        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = com;
        si.hStdOutput = com;
        si.hStdError  = com;

        /* argv[0]=busybox (multi-call) + "sh" "-i" -> shell interativo na serial */
        cmd[0] = '"';
        lstrcpyA(cmd + 1, busybox);
        lstrcatA(cmd, "\" sh -i");

        ZeroMemory(&pi, sizeof pi);
        if (CreateProcessA(busybox, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            DWORD ec = 0;
            /* banner SO depois do spawn ok (senao mentia); WriteFile nao bloqueia
             * mais — flow-control foi limpo no DCB */
            WriteFile(com, banner, (DWORD)lstrlenA(banner), &w, NULL);
            logln("ntdbgcon: shell spawnado", 0xFFFFFFFFu);
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &ec);   /* review: logar o exit code (0xC0000135=DLL faltando) */
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            logln("ntdbgcon: shell saiu (exit)", ec);
        } else {
            logln("ntdbgcon: CreateProcess(busybox) falhou", GetLastError());
            logln(busybox, 0xFFFFFFFFu);
        }
        Sleep(500);
    }
}
