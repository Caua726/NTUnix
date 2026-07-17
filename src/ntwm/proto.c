/*
 * proto.c — I/O do protocolo (lado ntwm). Cliente sincrono do NTU_PIPE_DISPD.
 *
 * Conexao com retry (padrao do ntctl: WaitNamedPipeA em ERROR_PIPE_BUSY) e
 * troca para READMODE_MESSAGE. Uma mensagem por Read/Write (message-mode).
 */
#include "ntwm.h"
#include <stdarg.h>

HANDLE g_pipe = INVALID_HANDLE_VALUE;

int wm_connect(void)
{
    for (int i = 0; i < 50; i++) {
        g_pipe = CreateFileA(NTU_PIPE_DISPD, GENERIC_READ | GENERIC_WRITE,
                             0, NULL, OPEN_EXISTING, 0, NULL);
        if (g_pipe != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(NTU_PIPE_DISPD, 2000);
            continue;
        }
        Sleep(200);   /* dispd talvez ainda subindo */
    }
    if (g_pipe == INVALID_HANDLE_VALUE)
        return -1;

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(g_pipe, &mode, NULL, NULL);
    return 0;
}

void wm_send(const char *fmt, ...)
{
    if (g_pipe == INVALID_HANDLE_VALUE)
        return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    DWORD w;
    WriteFile(g_pipe, buf, (DWORD)n, &w, NULL);
}

int wm_read(char *buf, int cap)
{
    if (g_pipe == INVALID_HANDLE_VALUE)
        return -1;
    /* remonta fragmentos: uma mensagem maior que o buffer chega em varios
     * ReadFile com ERROR_MORE_DATA — sem juntar, viravam eventos falsos (#77) */
    int total = 0;
    for (;;) {
        DWORD n = 0;
        BOOL ok = ReadFile(g_pipe, buf + total, (DWORD)(cap - 1 - total), &n, NULL);
        total += (int)n;
        if (ok)
            return total;                       /* mensagem completa */
        if (GetLastError() == ERROR_MORE_DATA) {
            if (total >= cap - 1)
                return total;                   /* buffer cheio: entrega o que cabe */
            continue;                           /* le o resto da mensagem */
        }
        return total > 0 ? total : -1;
    }
}
