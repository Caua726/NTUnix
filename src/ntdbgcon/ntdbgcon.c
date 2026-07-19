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
 * NAO deve entrar em build de producao (canal sem autenticacao). Gate no unit.
 */
#include <windows.h>

int main(void)
{
    HANDLE com;
    DCB dcb;
    COMMTIMEOUTS to;
    char path[MAX_PATH], *slash;
    DWORD n;

    com = CreateFileA("\\\\.\\COM1", GENERIC_READ | GENERIC_WRITE,
                      0, NULL, OPEN_EXISTING, 0, NULL);
    if (com == INVALID_HANDLE_VALUE)
        return 1;

    ZeroMemory(&dcb, sizeof dcb);
    dcb.DCBlength = sizeof dcb;
    if (GetCommState(com, &dcb)) {
        dcb.BaudRate = CBR_115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary  = TRUE;
        SetCommState(com, &dcb);
    }
    ZeroMemory(&to, sizeof to);
    to.ReadIntervalTimeout        = MAXDWORD;       /* bloqueia ate >=1 byte */
    to.ReadTotalTimeoutMultiplier = MAXDWORD;
    to.ReadTotalTimeoutConstant   = MAXDWORD - 1;
    SetCommTimeouts(com, &to);
    SetHandleInformation(com, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    /* busybox.exe fica no mesmo diretorio deste launcher (/system/bin) */
    n = GetModuleFileNameA(NULL, path, sizeof path);
    if (n == 0 || n >= sizeof path)
        return 1;
    slash = path;
    { char *p; for (p = path; *p; p++) if (*p == '\\' || *p == '/') slash = p; }
    lstrcpyA(slash + 1, "busybox.exe");

    for (;;) {
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[MAX_PATH + 16];

        ZeroMemory(&si, sizeof si);
        si.cb = sizeof si;
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdInput  = com;
        si.hStdOutput = com;
        si.hStdError  = com;

        /* argv[0]=busybox (multi-call) + "sh" "-i" -> shell interativo na serial */
        cmd[0] = '"';
        lstrcpyA(cmd + 1, path);
        lstrcatA(cmd, "\" sh -i");

        ZeroMemory(&pi, sizeof pi);
        if (CreateProcessA(path, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            const char *banner = "\r\n[ntdbgcon] shell serial pronto\r\n";
            DWORD w;
            WriteFile(com, banner, (DWORD)lstrlenA(banner), &w, NULL);
            WaitForSingleObject(pi.hProcess, INFINITE);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        Sleep(500);   /* shell saiu -> reabre */
    }
}
