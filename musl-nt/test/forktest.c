/* Teste do fork() estilo-Cygwin (Caminho B). Roda no Wine porque só usa Win32
 * padrão (CreateProcessW/WriteProcessMemory/SetThreadContext). */
#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    pid_t p;
    int st = 0;

    write(1, "before-fork\n", 12);

    p = fork();
    if (p < 0) {
        write(1, "fork-FAILED\n", 12);
        return 1;
    }
    if (p == 0) {
        /* filho: memória copiada, retomado no ponto do fork */
        char b[64];
        int n = snprintf(b, sizeof b, "CHILD pid=%d ppid=%d\n",
                         (int)getpid(), (int)getppid());
        write(1, b, n);
        _exit(42);
    }
    {
        char b[64];
        int n = snprintf(b, sizeof b, "PARENT child=%d\n", (int)p);
        write(1, b, n);
    }
    if (waitpid(p, &st, 0) == p)
        printf("PARENT: child status=%d\n", WEXITSTATUS(st));
    else
        printf("PARENT: wait FAILED\n");
    fflush(stdout);
    return 0;
}
