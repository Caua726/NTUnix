/*
 * demod.c — serviço de demonstração.
 *
 * Bate um heartbeat a cada 3 s no stdout (vai para /var/log/demod.log via
 * initd) e envia a mesma linha ao logd pelo pipe ntunix-logd, provando o
 * ciclo completo: supervisao + captura de log + IPC entre servicos.
 */
#include "../common/ntu.h"

static void send_logd(const char *msg)
{
    HANDLE h = CreateFileA(NTU_PIPE_LOGD, GENERIC_WRITE, 0, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return; /* logd ocupado ou fora do ar — heartbeat local ainda vale */
    DWORD w;
    WriteFile(h, msg, (DWORD)strlen(msg), &w, NULL);
    WriteFile(h, "\n", 1, &w, NULL);
    CloseHandle(h);
}

int main(void)
{
    printf("demod %s: iniciado (pid %lu)\n", NTU_VERSION, GetCurrentProcessId());
    fflush(stdout);

    for (unsigned long i = 1;; i++) {
        char ts[32], msg[128];
        ntu_now(ts, sizeof ts);
        snprintf(msg, sizeof msg, "demod: heartbeat #%lu", i);
        printf("[%s] %s\n", ts, msg);
        fflush(stdout);
        send_logd(msg);
        Sleep(3000);
    }
}
