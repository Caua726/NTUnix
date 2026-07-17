/*
 * ntuini.c — parser mínimo de units no estilo systemd (.service).
 *
 * Suporta: [Secao], chave=valor, comentários com '#' ou ';'.
 * Sem continuação de linha, sem quoting — suficiente para a v0.
 */
#include "ntu.h"

int ntu_ini_parse(const char *win_path, ntu_ini_fn fn, void *ud)
{
    FILE *f = fopen(win_path, "r");
    if (!f)
        return -1;

    char section[64] = "";
    char line[1024];

    while (fgets(line, sizeof line, f)) {
        /* linha maior que o buffer: descarta o resto e rejeita, para nao virar
         * varias "linhas" com chaves/valores parciais (#48) */
        if (!strchr(line, '\n') && !feof(f)) {
            int ch;
            while ((ch = fgetc(f)) != '\n' && ch != EOF)
                ;
            continue;
        }
        ntu_trim(line);
        if (!line[0] || line[0] == '#' || line[0] == ';')
            continue;

        if (line[0] == '[') {
            char *end = strchr(line, ']');
            if (!end) continue;
            *end = 0;
            strncpy(section, line + 1, sizeof section - 1);
            section[sizeof section - 1] = 0;
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = line, *val = eq + 1;
        ntu_trim(key);
        ntu_trim(val);
        if (key[0])
            fn(section, key, val, ud);
    }
    fclose(f);
    return 0;
}
