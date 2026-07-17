/*
 * ntctl.c — cliente de controle do initd (VISAO.md §10).
 *
 *   ntctl list | ping | reload | shutdown
 *   ntctl start|stop|restart|status|enable|disable <servico>
 *   ntctl logs <servico> [linhas]
 *
 * Fala o protocolo texto do pipe ntunix-initd e imprime a resposta crua.
 * Exit code: 0 = OK, 1 = ERR do initd, 2 = initd inacessivel, 3 = uso.
 */
#include "../common/ntu.h"
#include <ctype.h>

static void usage(void)
{
    fprintf(stderr,
        "uso: ntctl <comando> [args]\n"
        "\n"
        "  ntctl list                     lista servicos\n"
        "  ntctl status <servico>         detalhes de um servico\n"
        "  ntctl start <servico>          inicia (com dependencias)\n"
        "  ntctl stop <servico>           para (mata o job inteiro)\n"
        "  ntctl restart <servico>        para e inicia\n"
        "  ntctl enable <servico>         inicia no boot do initd\n"
        "  ntctl disable <servico>        nao inicia no boot\n"
        "  ntctl logs <servico> [n]       ultimas n linhas do log (padrao 20)\n"
        "  ntctl reload                   rescaneia /etc/units\n"
        "  ntctl ping                     verifica o initd\n"
        "  ntctl shutdown                 encerra o initd e os servicos\n");
}

static HANDLE connect_initd(void)
{
    for (int i = 0; i < 5; i++) {
        HANDLE h = CreateFileA(NTU_PIPE_INITD, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(NTU_PIPE_INITD, 2000);
            continue;
        }
        break;
    }
    return INVALID_HANDLE_VALUE;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage();
        return 3;
    }

    /* monta "VERBO arg1 arg2..." em maiusculas no verbo */
    char cmd[1024];
    strncpy(cmd, argv[1], sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;
    for (char *c = cmd; *c; c++)
        *c = (char)toupper((unsigned char)*c);
    for (int i = 2; i < argc; i++) {
        strncat(cmd, " ", sizeof cmd - strlen(cmd) - 1);
        strncat(cmd, argv[i], sizeof cmd - strlen(cmd) - 1);
    }

    HANDLE h = connect_initd();
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "ntctl: initd nao esta rodando (pipe %s inacessivel, erro %lu)\n",
                NTU_PIPE_INITD, GetLastError());
        return 2;
    }

    DWORD w;
    if (!WriteFile(h, cmd, (DWORD)strlen(cmd), &w, NULL)) {
        fprintf(stderr, "ntctl: falha ao enviar comando (%lu)\n", GetLastError());
        CloseHandle(h);
        return 2;
    }

    char buf[8192];
    DWORD n;
    int first = 1, ok = 0;
    while (ReadFile(h, buf, sizeof buf - 1, &n, NULL) && n) {
        buf[n] = 0;
        if (first) {
            ok = (n >= 2 && buf[0] == 'O' && buf[1] == 'K');
            first = 0;
        }
        fputs(buf, stdout);
    }
    CloseHandle(h);
    return ok ? 0 : 1;
}
