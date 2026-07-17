/* Teste do posix_spawn NT-nativo (sem clone): spawn + wait + file_actions.
 *
 * A visão da NTUnix é rodar TANTO apps Unix buildados-para-ela QUANTO apps
 * Windows. Ambos são PE, e o posix_spawn usa CreateProcessW no processo atual
 * (não depende de fork/RtlCloneUserProcess), então roda os dois — e até no Wine.
 * nt_path_at resolve caminho Unix (/...) para NTUNIX_ROOT e caminho Windows
 * (C:\...) direto. */
#define _GNU_SOURCE
#include <spawn.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

int main(void)
{
    pid_t pid;
    int st = 0, rc, fd, n;
    char buf[128];

    /* 1) app Unix-para-NTUnix: hello.exe (irmão), stdout redirecionado p/
     *    arquivo via file_actions. */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "spawn_out.txt",
                                     O_WRONLY | O_CREAT | O_TRUNC, 0644);
    char *argv1[] = { "hello.exe", NULL };
    rc = posix_spawn(&pid, "hello.exe", &fa, NULL, argv1, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc) { printf("FAIL spawn unix-app: %s\n", strerror(rc)); return 1; }
    if (waitpid(pid, &st, 0) != pid) { printf("FAIL wait\n"); return 1; }
    printf("unix-app exit=%d\n", WEXITSTATUS(st));

    /* Esta linha vai para o CONSOLE, provando que nosso fd 1 foi restaurado
     * depois de o file_actions tê-lo apontado para o arquivo. */
    fd = open("spawn_out.txt", O_RDONLY);
    n = fd >= 0 ? (int)read(fd, buf, sizeof buf - 1) : -1;
    if (fd >= 0) close(fd);
    if (n <= 0) { printf("FAIL: redirect file vazio\n"); return 1; }
    buf[n] = 0;
    printf("unix-app redirect capturou %d bytes\n", n);

    /* 2) app Windows: cmd.exe (best-effort — depende do ambiente ter cmd).
     *    Prova exec cross-ABI: o mesmo posix_spawn roda um binário Win32. */
    char *argv2[] = { "cmd.exe", "/d", "/c", "echo windows-app-ok", NULL };
    rc = posix_spawn(&pid, "C:\\windows\\system32\\cmd.exe", NULL, NULL,
                     argv2, environ);
    if (rc == 0 && waitpid(pid, &st, 0) == pid)
        printf("windows-app exit=%d\n", WEXITSTATUS(st));
    else
        printf("windows-app: indisponivel neste ambiente (rc=%d)\n", rc);

    printf("spawn-test: PASS\n");
    return 0;
}
