/*
 * ntu.h — definições compartilhadas do NTUnix (Fase 1, hospedado sobre Win32).
 *
 * Convenções:
 *  - Caminhos "unix-style" (/etc/..., /var/...) são traduzidos para
 *    <NTUNIX_ROOT>\... via ntu_path(). Ver docs/VISAO.md §12.
 *  - Strings emitidas em runtime são ASCII puro (sem acentos) para não
 *    depender de codepage de console.
 */
#ifndef NTU_H
#define NTU_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NTU_VERSION    "0.1.0-dev"
#define NTU_PIPE_INITD "\\\\.\\pipe\\ntunix-initd"
#define NTU_PIPE_LOGD  "\\\\.\\pipe\\ntunix-logd"
#define NTU_PIPE_DISPD "\\\\.\\pipe\\ntunix-dispd"
#define NTU_PIPE_DISPD_APP "\\\\.\\pipe\\ntunix-dispd-app"

/* ntupath.c — raiz e tradução de caminhos */
const char *ntu_root(void);                                  /* raiz NTUnix, sem barra final */
void ntu_path(const char *unix_path, char *out, size_t cap); /* /etc/x → <root>\etc\x        */
void ntu_ensure_dir(const char *win_path);                   /* mkdir -p                     */

/* ntuini.c — parser mínimo de arquivos .ini/.service */
typedef void (*ntu_ini_fn)(const char *section, const char *key,
                         const char *value, void *ud);
int ntu_ini_parse(const char *win_path, ntu_ini_fn fn, void *ud);

/* ntuutil.c */
void ntu_trim(char *s);                    /* tira espacos/CR/LF das pontas   */
void ntu_now(char *out, size_t cap);       /* "2026-07-16 16:55:03"           */

#endif
