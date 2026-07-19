/*
 * dbgterm.c — terminal de debug COMPARTILHADO (SO EM DEV, NTUNIX_DEBUG=1).
 *
 * Abre um terminal VISIVEL no desktop e faz uma ponte de rede: uma thread
 * reverse-conecta no host (10.0.2.2:2323, gateway do SLIRP) e injeta os bytes
 * recebidos como se fossem digitados nesse terminal (term_input) — entao os
 * comandos do desenvolvedor remoto APARECEM digitando + executando na tela da VM,
 * ao vivo. A saida do terminal (stream VT) e' tapada em vt_feed e mandada de volta
 * pelo socket, pra o lado remoto ver o resultado.
 *
 * E' o MESMO terminal, mesmo shell, mesmo tty do desktop — nao um shell separado.
 * O usuario ve tudo; o remoto dirige. Canal sem auth -> so em build de dev.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include "dispd.h"

#define DBG_HOST "10.0.2.2"
#define DBG_PORT 2323

static Window        *g_dbgwin;                 /* a janela do terminal de debug */
static volatile SOCKET g_dbgsock = INVALID_SOCKET;
static volatile LONG   g_connected;

/* tap: chamado de vt_feed (thread leitora do terminal) — manda a saida do terminal
 * de debug pro socket, pro lado remoto ver. Best-effort; nao trava o render. */
void dbgterm_tap(Terminal *t, const char *bytes, int n)
{
    SOCKET s;
    if (!g_dbgwin || t != g_dbgwin->term) return;
    if (!g_connected) return;
    s = g_dbgsock;
    if (s != INVALID_SOCKET && n > 0)
        send(s, bytes, n, 0);
}

static DWORD WINAPI bridge_thread(LPVOID arg)
{
    WSADATA wsa;
    (void)arg;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    for (;;) {
        SOCKET s;
        struct sockaddr_in sa;
        char buf[1024];
        const char *banner =
            "\r\n[dispd] terminal de debug COMPARTILHADO conectado.\r\n"
            "Os comandos do assistente aparecem aqui e rodam neste terminal.\r\n";

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) { Sleep(2000); continue; }
        memset(&sa, 0, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(DBG_PORT);
        sa.sin_addr.s_addr = inet_addr(DBG_HOST);
        if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
            closesocket(s); Sleep(2000); continue;
        }
        g_dbgsock = s;
        InterlockedExchange(&g_connected, 1);
        send(s, banner, (int)strlen(banner), 0);

        /* socket -> term_input: injeta no terminal visivel (aparece + roda). O
         * Enter chega como \n; o terminal do desktop usa \r (CR) -> convertemos,
         * pois o pty faz ICRNL (CR->NL). Assim casa com o teclado. */
        for (;;) {
            int r = recv(s, buf, sizeof buf, 0), i;
            if (r <= 0) break;
            for (i = 0; i < r; i++)
                if (buf[i] == '\n') buf[i] = '\r';
            if (g_dbgwin && g_dbgwin->term)
                term_input(g_dbgwin->term, buf, r);
        }

        InterlockedExchange(&g_connected, 0);
        g_dbgsock = INVALID_SOCKET;
        closesocket(s);
        Sleep(1000);   /* reconecta */
    }
}

/* chamado no boot do dispd (dispd.c). Retorna 1 se ativou o terminal de debug
 * (build de dev), 0 se nao (o dispd abre o terminal normal). Gate = arquivo
 * marcador /etc/ntunix-debug, criado pelo stage-files so em dev (NTUNIX_DEBUG=1) —
 * o env NTUNIX_DEBUG e' de BUILD, nao existe no runtime da VM. */
int dbgterm_start(void)
{
    char marker[512];
    ntu_path("/etc/ntunix-debug", marker, sizeof marker);
    if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES)
        return 0;                     /* nao e' build de dev */
    g_dbgwin = spawn_terminal(0);     /* terminal visivel, shell padrao */
    if (!g_dbgwin) { dispd_log("dbgterm: spawn_terminal falhou"); return 1; }
    CreateThread(0, 0, bridge_thread, 0, 0, 0);
    dispd_log("dbgterm: terminal de debug compartilhado ativo (rede 10.0.2.2:%d)", DBG_PORT);
    return 1;
}
