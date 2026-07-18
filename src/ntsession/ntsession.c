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

static BOOL dispd_up(void)
{
    /* audit #78: NAO conecta no pipe do WM (instancia unica) — o CreateFile antigo
     * roubava o slot do ntwm quando o pipe estava livre (boot/restart do WM) e
     * gerava conexao-fantasma no dispd. WaitNamedPipe so CHECA se o pipe existe,
     * sem ocupar a instancia. */
    if (WaitNamedPipeA(NTU_PIPE_DISPD, 0))
        return TRUE;                                   /* instancia livre -> up */
    return GetLastError() != ERROR_FILE_NOT_FOUND;     /* existe mas ocupado = up */
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

    /* Sessao grafica: o desktop (dispd + ntwm) sobe pelo initd, pois as units
     * estao habilitadas. O ntsession CEDE a tela: so vigia. Se o dispd nao
     * subir, cai num terminal de recuperacao para a sessao nunca ficar sem
     * shell (via cmd /K, que sobrevive a saida do ash). */
    char term[MAX_PATH + 128];
    snprintf(term, sizeof term,
             "cmd.exe /K \"cd /d %s && title NTUnix %s (recuperacao) && system\\bin\\busybox.exe ash -i\"",
             ntu_root(), NTU_VERSION);

    for (;;) {
        /* espera o desktop (ate ~15s apos o boot / restart) */
        BOOL up = FALSE;
        for (int i = 0; i < 150 && !up; i++) {
            if (dispd_up()) up = TRUE;
            else Sleep(100);
        }
        if (up) {
            /* desktop no ar: idle enquanto viver (initd reinicia o dispd) */
            while (dispd_up())
                Sleep(1000);
            continue;   /* caiu: reavalia (pode voltar via initd) */
        }
        /* recuperacao: dispd nao subiu -> terminal ate ele voltar */
        PROCESS_INFORMATION pi;
        if (spawn(term, CREATE_NEW_CONSOLE, &pi)) {
            /* audit #6: espera o console OU o desktop voltar — o que vier antes.
             * Antes o WaitForSingleObject(INFINITE) deixava o console de
             * recuperacao pendurado SOBRE o desktop mesmo depois do dispd voltar. */
            for (;;) {
                if (WaitForSingleObject(pi.hProcess, 500) == WAIT_OBJECT_0)
                    break;                      /* console encerrou */
                if (dispd_up()) {               /* desktop no ar -> cede a tela */
                    TerminateProcess(pi.hProcess, 0);
                    WaitForSingleObject(pi.hProcess, 2000);
                    break;
                }
            }
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        Sleep(500);
    }
}
