/*
 * logd.c — coletor central de logs, mínimo viável (VISAO.md §10).
 *
 * Recebe linhas de texto no pipe ntunix-logd e as grava com timestamp em
 * /var/log/system.log. Um cliente por vez (suficiente para a v0); clientes
 * conectam, escrevem e desconectam — ver send_logd() no demod.
 *
 * O stdout do logd vai para /var/log/logd.log via redirecao do initd.
 */
#include "../common/ntu.h"

static HANDLE g_syslog;

static void append_line(const char *line)
{
    char ts[32], rec[4300];
    ntu_now(ts, sizeof ts);
    int n = snprintf(rec, sizeof rec, "[%s] %s\n", ts, line);
    DWORD w;
    WriteFile(g_syslog, rec, (DWORD)n, &w, NULL);
}

int main(void)
{
    char sp[MAX_PATH];
    ntu_path("/var/log/system.log", sp, sizeof sp);

    g_syslog = CreateFileA(sp, FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_syslog == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "logd: nao consegui abrir %s (%lu)\n", sp, GetLastError());
        return 1;
    }

    HANDLE pipe = CreateNamedPipeA(NTU_PIPE_LOGD, PIPE_ACCESS_INBOUND,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                   1, 65536, 65536, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "logd: pipe falhou (%lu) — outro logd rodando?\n", GetLastError());
        return 1;
    }

    printf("logd %s: ouvindo em %s, log central em %s\n",
           NTU_VERSION, NTU_PIPE_LOGD, sp);
    fflush(stdout);
    append_line("logd: iniciado");

    unsigned long total = 0;
    for (;;) {
        BOOL con = ConnectNamedPipe(pipe, NULL)
                       ? TRUE
                       : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!con)
            continue;

        char buf[4096];
        DWORD n;
        while (ReadFile(pipe, buf, sizeof buf - 1, &n, NULL) && n) {
            buf[n] = 0;
            for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
                ntu_trim(line);
                if (*line) {
                    append_line(line);
                    total++;
                }
            }
        }
        DisconnectNamedPipe(pipe);

        if (total && total % 100 == 0) {
            printf("logd: %lu mensagens gravadas\n", total);
            fflush(stdout);
        }
    }
}
