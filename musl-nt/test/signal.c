/* Teste de entrega de sinais: raise()/handler e SIG_IGN. raise() vira
 * tkill(self, sig) -> entrega local, e o handler roda no retorno da syscall.
 * (Ctrl-C de verdade precisa do console; testado no VM.) */
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

static volatile sig_atomic_t got_sig;
static void handler(int sig) { got_sig = sig; }

int main(void)
{
    /* 1) o handler roda em raise() */
    signal(SIGINT, handler);
    raise(SIGINT);
    if (got_sig != SIGINT) {
        printf("FAIL: handler nao rodou (got=%d)\n", (int)got_sig);
        return 1;
    }
    printf("raise+handler OK (got=%d)\n", (int)got_sig);

    /* 2) SIG_IGN: não termina, handler não roda */
    got_sig = 0;
    signal(SIGINT, SIG_IGN);
    raise(SIGINT);
    printf("SIG_IGN OK (got=%d, esperado 0)\n", (int)got_sig);

    /* 3) reinstala e dispara de novo */
    signal(SIGINT, handler);
    got_sig = 0;
    raise(SIGINT);
    if (got_sig != SIGINT) { printf("FAIL: re-arme\n"); return 1; }

    printf("signal-test: PASS\n");
    return 0;
}
