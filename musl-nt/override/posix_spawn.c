/*
 * NTUnix posix_spawn: substitui o da musl (que faz clone+exec por dentro, e o
 * exec crasha dentro do clone RtlCloneUserProcess). Aqui NÃO há clone: as
 * file_actions são aplicadas nos fds do PRÓPRIO processo, faz-se o spawn
 * (CreateProcessW no processo atual, via a pseudo-syscall NT_SYS_spawn) e os
 * fds são restaurados. É o caminho correto para rodar binários externos reais.
 *
 * attr (sinais/pgroup/ids/sched) é ignorado: a NTUnix v0 não tem esses modelos.
 * posix_spawnp cai aqui via attr->__fn, mas a busca em PATH não é feita
 * (limitação v0: passe o caminho do executável, não só o nome).
 */
#define _GNU_SOURCE
#include <spawn.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include "syscall.h"

/* Deve casar com include/nt/ntabi.h: NT_SYS_spawn. */
#define NT_NR_spawn 0x1000000

/* Espelho de src/process/fdop.h (copiado para não puxar os #define de malloc
 * daquele header interno). */
#define FDOP_CLOSE 1
#define FDOP_DUP2 2
#define FDOP_OPEN 3
#define FDOP_CHDIR 4
#define FDOP_FCHDIR 5
struct fdop {
	struct fdop *next, *prev;
	int cmd, fd, srcfd, oflag;
	mode_t mode;
	char path[];
};

extern char **__environ;

int posix_spawn(pid_t *restrict res, const char *restrict path,
	const posix_spawn_file_actions_t *fa,
	const posix_spawnattr_t *restrict attr,
	char *const argv[restrict], char *const envp[restrict])
{
	struct { int fd, backup; } saved[64];
	int nsaved = 0, i, has_chdir = 0;
	char cwd[4096];
	long cwdlen = -1;
	long pid;
	(void)attr;

	if (fa && fa->__actions) {
		struct fdop *op = fa->__actions;
		for (op = fa->__actions; op->next; op = op->next)
			; /* vai até o fim da lista */
		/* checa se há chdir/fchdir para salvar/restaurar o CWD do pai */
		{
			struct fdop *q;
			for (q = fa->__actions; q; q = q->next)
				if (q->cmd == FDOP_CHDIR || q->cmd == FDOP_FCHDIR)
					has_chdir = 1;
		}
		if (has_chdir)
			cwdlen = __syscall(SYS_getcwd, cwd, sizeof cwd);

		/* aplica na mesma ordem da musl: do fim para o começo (->prev) */
		for (; op; op = op->prev) {
			int target = (op->cmd == FDOP_CHDIR ||
			              op->cmd == FDOP_FCHDIR) ? -1 : op->fd;
			/* salva (uma vez) o fd-alvo antes de sobrescrevê-lo */
			if (target >= 0 && nsaved < (int)(sizeof saved / sizeof *saved)) {
				int k, already = 0;
				for (k = 0; k < nsaved; k++)
					if (saved[k].fd == target) { already = 1; break; }
				if (!already) {
					int b = __syscall(SYS_fcntl, target,
					                  F_DUPFD_CLOEXEC, 40);
					saved[nsaved].fd = target;
					saved[nsaved].backup = b < 0 ? -1 : b;
					nsaved++;
				}
			}
			switch (op->cmd) {
			case FDOP_CLOSE:
				__syscall(SYS_close, op->fd);
				break;
			case FDOP_DUP2:
				if (op->srcfd != op->fd) {
					__syscall(SYS_dup3, op->srcfd, op->fd, 0);
				} else {
					int fl = __syscall(SYS_fcntl, op->fd, F_GETFD);
					__syscall(SYS_fcntl, op->fd, F_SETFD,
					          fl & ~FD_CLOEXEC);
				}
				break;
			case FDOP_OPEN: {
				int t = __syscall(SYS_openat, AT_FDCWD, op->path,
				                  op->oflag, op->mode);
				if (t >= 0 && t != op->fd) {
					__syscall(SYS_dup3, t, op->fd, 0);
					__syscall(SYS_close, t);
				}
				break; }
			case FDOP_CHDIR:
				__syscall(SYS_chdir, op->path);
				break;
			case FDOP_FCHDIR:
				__syscall(SYS_fchdir, op->fd);
				break;
			}
		}
	}

	/* spawn no processo atual (o pai) — o filho herda os fds 0/1/2 montados */
	pid = __syscall(NT_NR_spawn, path, argv, envp ? envp : __environ);

	/* restaura os fds tocados (ordem inversa) e o CWD */
	for (i = nsaved - 1; i >= 0; i--) {
		if (saved[i].backup >= 0) {
			__syscall(SYS_dup3, saved[i].backup, saved[i].fd, 0);
			__syscall(SYS_close, saved[i].backup);
		} else {
			__syscall(SYS_close, saved[i].fd);
		}
	}
	if (has_chdir && cwdlen > 0)
		__syscall(SYS_chdir, cwd);

	if (pid < 0)
		return (int)-pid;   /* posix_spawn devolve o errno positivo */
	if (res)
		*res = (pid_t)pid;
	return 0;
}
