/*
 * npx.h — libntposix: interface POSIX-like sobre o NT (VISAO.md §6).
 *
 * v0: backend Win32 (CreateFile/ReadFile/...), como manda a VISAO §26
 * ("tudo inicialmente sobre Win32, migrando gradualmente para a Native
 * API"). A INTERFACE aqui e o contrato estavel; o backend migra funcao
 * por funcao para ntdll (NtCreateFile/NtReadFile/...) sem mexer nos
 * chamadores. Ver docs/pesquisa/nt-native-api.md para a ordem de ataque.
 *
 * Descritores: fd inteiros pequenos (0/1/2 = stdin/out/err), tabela
 * propria fd->HANDLE estilo dtable do Cygwin — NAO da pra usar HANDLE
 * cru como fd (pesquisa §7).
 */
#ifndef NPX_H
#define NPX_H

#include <stddef.h>

typedef long long npx_ssize;
typedef long long npx_off;

/* flags de npx_open (subset POSIX) */
#define NPX_O_RDONLY   0x0000
#define NPX_O_WRONLY   0x0001
#define NPX_O_RDWR     0x0002
#define NPX_O_CREAT    0x0100
#define NPX_O_TRUNC    0x0200
#define NPX_O_APPEND   0x0400
#define NPX_O_EXCL     0x0800

/* whence de npx_lseek */
#define NPX_SEEK_SET   0
#define NPX_SEEK_CUR   1
#define NPX_SEEK_END   2

/* arquivos e fds */
int       npx_open(const char *path, int flags);
npx_ssize npx_read(int fd, void *buf, size_t n);
npx_ssize npx_write(int fd, const void *buf, size_t n);
npx_off   npx_lseek(int fd, npx_off off, int whence);
int       npx_close(int fd);
int       npx_dup2(int oldfd, int newfd);

/* diretorios */
typedef struct {
    char name[260];
    int  is_dir;
    unsigned long long size;
} npx_dirent;

typedef struct NpxDir NpxDir;
NpxDir     *npx_opendir(const char *path);
int         npx_readdir(NpxDir *d, npx_dirent *out); /* 1=ok, 0=fim, -1=erro */
void        npx_closedir(NpxDir *d);

/* processos (posix_spawn simplificado — pesquisa §5: sem fork generico) */
/* argv[0] = programa (path unix-style ou nome em /system/bin); NULL-terminado.
 * Se in/out/err >= 0, redireciona esses fds no filho. Retorna pid (>0) ou -1. */
long npx_spawn(char *const argv[], int in_fd, int out_fd, int err_fd);
int  npx_waitpid(long pid, int *exit_code); /* bloqueia; 0=ok, -1=erro */

/* utilidades de ambiente */
int  npx_chdir(const char *path);
char *npx_getcwd(char *buf, size_t cap); /* em termos unix-style */

/* ultimo erro legivel (backend-agnostico) */
const char *npx_strerror(void);

#endif
