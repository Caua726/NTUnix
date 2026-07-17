/*
 * ntupath.c — semente da camada de caminhos do NTUnix.
 *
 * Resolve a raiz do sistema (NTUNIX_ROOT) e traduz caminhos unix-style
 * para caminhos Win32. É deliberadamente simples: a versão completa vai
 * morar na libntposix.
 */
#include "ntu.h"

static char g_root[MAX_PATH];

static int ends_with_icase(const char *s, const char *suf)
{
    size_t ls = strlen(s), lf = strlen(suf);
    return ls >= lf && _stricmp(s + ls - lf, suf) == 0;
}

const char *ntu_root(void)
{
    if (g_root[0])
        return g_root;

    const char *env = getenv("NTUNIX_ROOT");
    if (env && *env) {
        strncpy(g_root, env, sizeof g_root - 1);
    } else {
        /* binarios NTUnix moram em <root>\system\bin — sobe dois niveis */
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *p = strrchr(exe, '\\');
        if (p) *p = 0; /* diretorio do exe */
        if (ends_with_icase(exe, "\\system\\bin")) {
            exe[strlen(exe) - strlen("\\system\\bin")] = 0;
            strncpy(g_root, exe, sizeof g_root - 1);
        } else {
            GetCurrentDirectoryA(sizeof g_root, g_root);
        }
    }

    /* normaliza: sem barra final */
    size_t n = strlen(g_root);
    while (n > 3 && (g_root[n - 1] == '\\' || g_root[n - 1] == '/'))
        g_root[--n] = 0;
    return g_root;
}

void ntu_path(const char *up, char *out, size_t cap)
{
    if (!up || !*up || cap == 0) {
        if (cap) out[0] = 0;
        return;
    }
    /* caminho Win32 (C:\...) ou UNC (\\srv\...) passa direto */
    if (up[1] == ':' || (up[0] == '\\' && up[1] == '\\')) {
        strncpy(out, up, cap - 1);
        out[cap - 1] = 0;
        return;
    }
    if (up[0] == '/') {
        snprintf(out, cap, "%s%s", ntu_root(), up);
    } else {
        strncpy(out, up, cap - 1);
        out[cap - 1] = 0;
    }
    for (char *c = out; *c; c++)
        if (*c == '/') *c = '\\';
}

void ntu_ensure_dir(const char *wpath)
{
    char tmp[MAX_PATH];
    strncpy(tmp, wpath, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = 0;

    /* pula o prefixo "C:\" ou "\\" */
    char *start = tmp;
    if (tmp[1] == ':') start = tmp + 3;
    else if (tmp[0] == '\\' && tmp[1] == '\\') start = tmp + 2;

    for (char *p = start; *p; p++) {
        if (*p == '\\') {
            *p = 0;
            CreateDirectoryA(tmp, NULL);
            *p = '\\';
        }
    }
    CreateDirectoryA(tmp, NULL);
}
