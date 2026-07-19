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

    /* abre a COM1, com retry (o driver serial pode demorar a subir no boot) */
    com = INVALID_HANDLE_VALUE;
    for (tries = 0; tries < 30; tries++) {
        com = CreateFileA("\\\\.\\COM1", GENERIC_READ | GENERIC_WRITE,
                          0, NULL, OPEN_EXISTING, 0, NULL);
        if (com != INVALID_HANDLE_VALUE) break;
        logln("ntdbgcon: CreateFile(COM1) falhou", GetLastError());
        Sleep(1000);
    }
    if (com == INVALID_HANDLE_VALUE) {
        logln("ntdbgcon: DESISTINDO da COM1 (sem serial no guest?)", 0xFFFFFFFFu);
        return 1;
    }
    logln("ntdbgcon: COM1 aberta OK", 0xFFFFFFFFu);

    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(com, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary  = TRUE;
        SetCommState(com, &dcb);
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

        WriteFile(com, banner, (DWORD)lstrlenA(banner), &w, NULL);

        ZeroMemory(&pi, sizeof pi);
        if (CreateProcessA(busybox, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            logln("ntdbgcon: shell spawnado", 0xFFFFFFFFu);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            logln("ntdbgcon: shell saiu — respawn", 0xFFFFFFFFu);
        } else {
            logln("ntdbgcon: CreateProcess(busybox) falhou", GetLastError());
            logln(busybox, 0xFFFFFFFFu);
        }
        Sleep(500);
    }
}
