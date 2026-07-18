/*
 * pipesrv.c — servidor de comandos do initd.
 *
 * Protocolo (texto puro, uma requisicao por conexao):
 *   cliente escreve:  "VERBO [arg1 [arg2]]"
 *   initd responde:   primeira linha "OK ..." ou "ERR ..." + corpo opcional
 *   e desconecta. Ver docs/PROTOCOLO.md.
 *
 * Single-threaded de proposito: comandos sao rapidos e a v0 nao precisa
 * de concorrencia aqui.
 */
#include "initd.h"

static char g_resp[131072];
static size_t g_len;

/* audit #76: snprintf devolve o que TERIA escrito; somar isso a g_len avancava
 * ALEM do buffer (g_len > sizeof g_resp, e o WriteFile(g_len) lia OOB). Clampa
 * o incremento ao espaco real restante. */
static size_t resp_clamp(int n)
{
    if (n < 0) return 0;
    size_t room = g_len < sizeof g_resp ? sizeof g_resp - 1 - g_len : 0;
    return (size_t)n > room ? room : (size_t)n;
}
#define ADD(...) (g_len += resp_clamp(snprintf(g_resp + g_len, \
                        sizeof g_resp > g_len ? sizeof g_resp - g_len : 0, __VA_ARGS__)))

static const char *state_name(SvcState st)
{
    switch (st) {
    case ST_RUNNING: return "running";
    case ST_FAILED:  return "failed";
    default:         return "stopped";
    }
}

static void cmd_list(void)
{
    ADD("OK\n%-16s %-8s %-7s %-8s %s\n", "SERVICO", "ESTADO", "PID", "RESTARTS", "DESCRICAO");
    EnterCriticalSection(&g_lock);
    for (Service *s = g_services; s; s = s->next) {
        char pid[16] = "-";
        if (s->state == ST_RUNNING)
            snprintf(pid, sizeof pid, "%lu", s->pid);
        ADD("%-16s %-8s %-7s %-8d %s\n",
            s->name, state_name(s->state), pid, s->restarts, s->description);
    }
    LeaveCriticalSection(&g_lock);
}

static void cmd_status(Service *s)
{
    EnterCriticalSection(&g_lock);
    ADD("OK\n%s - %s\n", s->name, s->description[0] ? s->description : "(sem descricao)");
    ADD("  Estado:     %s\n", state_name(s->state));
    if (s->state == ST_RUNNING) {
        ADD("  PID:        %lu\n", s->pid);
        ADD("  Uptime:     %llus\n",
            (unsigned long long)(GetTickCount64() - s->started_at) / 1000);
    } else {
        ADD("  UltimoExit: %lu\n", s->last_exit);
    }
    ADD("  Restarts:   %d\n", s->restarts);
    ADD("  Enabled:    %s\n", s->enabled ? "sim" : "nao");
    ADD("  ExecStart:  %s\n", s->exec_start);
    if (s->memory_max)
        ADD("  MemoryMax:  %llu bytes\n", s->memory_max);
    ADD("  Unit:       %s\n", s->unit_path);
    ADD("  Log:        /var/log/%s.log\n", s->name);
    LeaveCriticalSection(&g_lock);
}

static void cmd_logs(Service *s, int nlines)
{
    if (nlines <= 0) nlines = 20;
    if (nlines > 500) nlines = 500;

    char rel[160], p[MAX_PATH];
    snprintf(rel, sizeof rel, "/var/log/%s.log", s->name);
    ntu_path(rel, p, sizeof p);

    HANDLE f = CreateFileA(p, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        ADD("ERR sem log para %s ainda\n", s->name);
        return;
    }
    /* le so a cauda do arquivo (ultimos 64 KB) */
    static char buf[65537];
    LARGE_INTEGER size, off;
    GetFileSizeEx(f, &size);
    off.QuadPart = size.QuadPart > 65536 ? size.QuadPart - 65536 : 0;
    SetFilePointerEx(f, off, NULL, FILE_BEGIN);
    DWORD n = 0;
    ReadFile(f, buf, 65536, &n, NULL);
    CloseHandle(f);
    buf[n] = 0;

    /* acha o inicio das ultimas nlines linhas */
    char *end = buf + n, *pos = end;
    int count = 0;
    while (pos > buf) {
        pos--;
        if (*pos == '\n' && pos != end - 1) {
            if (++count >= nlines) {
                pos++;
                break;
            }
        }
    }
    ADD("OK\n%.*s", (int)(end - pos), pos);
    if (n && end[-1] != '\n')
        ADD("\n");
}

static void handle_cmd(char *req)
{
    g_len = 0;

    char *cmd = strtok(req, " \t");
    char *a1 = strtok(NULL, " \t");
    char *a2 = strtok(NULL, " \t");
    if (!cmd) {
        ADD("ERR comando vazio\n");
        return;
    }

    if (!_stricmp(cmd, "PING")) {
        ADD("OK ntunix-initd %s (root=%s)\n", NTU_VERSION, ntu_root());
        return;
    }
    if (!_stricmp(cmd, "LIST")) {
        cmd_list();
        return;
    }
    if (!_stricmp(cmd, "RELOAD")) {
        int n = svc_scan_units();
        ADD("OK %d unit(s) nova(s) carregada(s)\n", n);
        return;
    }
    if (!_stricmp(cmd, "SHUTDOWN")) {
        ADD("OK encerrando initd\n");
        InterlockedExchange(&g_shutdown, 1);
        return;
    }

    /* comandos com servico */
    if (!a1) {
        ADD("ERR uso: %s <servico>\n", cmd);
        return;
    }
    Service *s = svc_find(a1);
    if (!s) {
        ADD("ERR servico desconhecido: %s\n", a1);
        return;
    }

    if (!_stricmp(cmd, "START")) {
        char err[512];
        if (svc_start(s, err, sizeof err) == 0) {
            EnterCriticalSection(&g_lock);
            ADD("OK %s rodando (pid %lu)\n", s->name, s->pid);
            LeaveCriticalSection(&g_lock);
        } else
            ADD("ERR %s\n", err);
    } else if (!_stricmp(cmd, "STOP")) {
        svc_stop(s);
        if (svc_wait_stopped(s, 5000) == 0)
            ADD("OK %s parado\n", s->name);
        else
            ADD("ERR %s nao parou em 5s\n", s->name);
    } else if (!_stricmp(cmd, "RESTART")) {
        svc_stop(s);
        if (svc_wait_stopped(s, 5000) != 0) {
            ADD("ERR %s nao parou em 5s\n", s->name);
            return;
        }
        char err[512];
        if (svc_start(s, err, sizeof err) == 0) {
            EnterCriticalSection(&g_lock);
            ADD("OK %s reiniciado (pid %lu)\n", s->name, s->pid);
            LeaveCriticalSection(&g_lock);
        } else
            ADD("ERR %s\n", err);
    } else if (!_stricmp(cmd, "STATUS")) {
        cmd_status(s);
    } else if (!_stricmp(cmd, "LOGS")) {
        cmd_logs(s, a2 ? atoi(a2) : 20);
    } else if (!_stricmp(cmd, "ENABLE")) {
        if (svc_set_enabled(s, 1) == 0)
            ADD("OK %s habilitado no boot\n", s->name);
        else
            ADD("ERR nao consegui criar marcador (%lu)\n", GetLastError());
    } else if (!_stricmp(cmd, "DISABLE")) {
        if (svc_set_enabled(s, 0) == 0)
            ADD("OK %s desabilitado no boot\n", s->name);
        else
            ADD("ERR nao consegui remover marcador (%lu)\n", GetLastError());
    } else {
        ADD("ERR comando desconhecido: %s\n", cmd);
    }
}

void pipe_server_run(void)
{
    HANDLE pipe = CreateNamedPipeA(NTU_PIPE_INITD, PIPE_ACCESS_DUPLEX,
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
                                   PIPE_REJECT_REMOTE_CLIENTS,   /* audit #117: sem clientes de rede */
                                   1, 65536, 65536, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) {
        ilog("pipe: criacao falhou (%lu) — ja tem um initd rodando?", GetLastError());
        return;
    }
    ilog("ouvindo em %s", NTU_PIPE_INITD);

    while (!g_shutdown) {
        BOOL con = ConnectNamedPipe(pipe, NULL)
                       ? TRUE
                       : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (!con)
            continue;

        /* audit #74: espera o comando com DEADLINE (poll) — um cliente que
         * conecta e fica MUDO nao pode travar o unico thread de controle do
         * initd (senao todo ntctl, ate o shutdown, fica indisponivel). Cliente
         * real manda na hora -> o primeiro peek ja acha, sem atraso. */
        char req[4096];
        DWORD n = 0, avail = 0, waited = 0;
        while (!g_shutdown && waited < 3000) {
            if (!PeekNamedPipe(pipe, NULL, 0, NULL, &avail, NULL))
                break;                 /* pipe quebrado (cliente sumiu) */
            if (avail > 0)
                break;
            Sleep(50);
            waited += 50;
        }
        if (avail > 0 && ReadFile(pipe, req, sizeof req - 1, &n, NULL) && n) {
            req[n] = 0;
            ntu_trim(req);
            handle_cmd(req);
            DWORD w;
            WriteFile(pipe, g_resp, (DWORD)g_len, &w, NULL);
        }
        FlushFileBuffers(pipe);        /* #75 (cliente que nao le) = follow-up */
        DisconnectNamedPipe(pipe);
    }
    CloseHandle(pipe);
}
