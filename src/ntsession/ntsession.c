/*
 * ntsession.c — shell da sessão NTUnix (substitui o explorer.exe).
 *
 * Instalado como Winlogon\Shell pelo SetupComplete.cmd da ISO tratada.
 * Responsabilidades (v0):
 *   1. exportar NTUNIX_ROOT e prefixar /system/bin no PATH da sessao;
 *   2. garantir o initd rodando (spawna se o pipe nao responder);
 *   3. abrir o terminal da sessao (cmd.exe por enquanto — shell proprio
 *      vem na Fase 3) e reabri-lo se fechar: shell de sessao nao morre.
 *
 * Compilado com -mwindows: nenhuma janela propria, so as que ele cria.
 */
#include "../common/ntu.h"

static BOOL initd_up(void)
{
    HANDLE h = CreateFileA(NTU_PIPE_INITD, GENERIC_READ | GENERIC_WRITE,
                           0, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_PIPE_BUSY; /* ocupado = vivo */
    CloseHandle(h);
    return TRUE;
}

static BOOL spawn(const char *cmdline, DWORD flags, PROCESS_INFORMATION *pi)
{
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(pi, sizeof *pi);

    char buf[1024];
    strncpy(buf, cmdline, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    return CreateProcessA(NULL, buf, NULL, NULL, FALSE, flags,
                          NULL, ntu_root(), &si, pi);
}

int main(void)
{
    char bin[MAX_PATH];
    ntu_path("/system/bin", bin, sizeof bin);

    /* ambiente da sessao: raiz + ntctl no PATH */
    SetEnvironmentVariableA("NTUNIX_ROOT", ntu_root());
    char oldpath[4096] = "", newpath[8192];
    GetEnvironmentVariableA("PATH", oldpath, sizeof oldpath);
    snprintf(newpath, sizeof newpath, "%s;%s", bin, oldpath);
    SetEnvironmentVariableA("PATH", newpath);

    /* garante o supervisor */
    if (!initd_up()) {
        char cmd[MAX_PATH + 16];
        snprintf(cmd, sizeof cmd, "\"%s\\initd.exe\"", bin);
        PROCESS_INFORMATION pi;
        if (spawn(cmd, CREATE_NO_WINDOW, &pi)) { /* log em /var/log/initd.log */
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        for (int i = 0; i < 100 && !initd_up(); i++)
            Sleep(100);
    }

    /* terminal da sessao, ressuscitado se fechar */
    char term[MAX_PATH + 64];
    snprintf(term, sizeof term,
             "cmd.exe /K \"cd /d %s && echo NTUnix %s — use ntctl list\"",
             ntu_root(), NTU_VERSION);
    for (;;) {
        PROCESS_INFORMATION pi;
        if (!spawn(term, CREATE_NEW_CONSOLE, &pi)) {
            Sleep(3000);
            continue;
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        Sleep(500);
    }
}
