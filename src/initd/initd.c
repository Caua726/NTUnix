/*
 * initd.c — supervisor de serviços do NTUnix (VISAO.md §10).
 *
 * Fase 1 (hospedado): roda como processo Win32 comum dentro de um Windows
 * normal. Carrega units de /etc/units, inicia as habilitadas (marcador em
 * /etc/units/enabled/<nome>) e atende comandos do ntctl via named pipe.
 */
#include "initd.h"
#include <stdarg.h>

volatile LONG g_shutdown = 0;
static CRITICAL_SECTION g_loglock;

/* log do proprio initd: console + /var/log/initd.log */
void ilog(const char *fmt, ...)
{
    char msg[1024], ts[32];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ntu_now(ts, sizeof ts);

    EnterCriticalSection(&g_loglock);
    printf("[%s] initd: %s\n", ts, msg);
    fflush(stdout);

    char p[MAX_PATH];
    ntu_path("/var/log/initd.log", p, sizeof p);
    FILE *f = fopen(p, "ab");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, msg);
        fclose(f);
    }
    LeaveCriticalSection(&g_loglock);
}

static BOOL WINAPI on_ctrl(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_shutdown, 1);
    ilog("sinal de encerramento recebido, parando servicos");
    svc_stop_all();
    ilog("initd encerrado");
    ExitProcess(0);
}

static void ensure_tree(void)
{
    static const char *dirs[] = {
        "/etc/units/enabled", "/var/log", "/run", "/system/bin",
    };
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        char p[MAX_PATH];
        ntu_path(dirs[i], p, sizeof p);
        ntu_ensure_dir(p);
    }
}

int main(void)
{
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_loglock);
    SetConsoleCtrlHandler(on_ctrl, TRUE);

    ensure_tree();
    ilog("ntunix-initd %s iniciando, root=%s", NTU_VERSION, ntu_root());

    int n = svc_scan_units();
    ilog("%d unit(s) carregada(s) de /etc/units", n);

    /* inicia servicos habilitados */
    for (Service *s = g_services; s; s = s->next) {
        if (!s->enabled)
            continue;
        char err[512];
        if (svc_start(s, err, sizeof err) != 0)
            ilog("%s: falha no boot: %s", s->name, err);
    }

    pipe_server_run();

    ilog("parando todos os servicos");
    svc_stop_all();
    ilog("initd encerrado");
    return 0;
}
