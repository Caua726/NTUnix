/*
 * sys_pty.c — pty slave do NTUnix (sem ConPTY).
 *
 * Implementa a disciplina de linha N_TTY (modelo do drivers/tty/n_tty.c do
 * Linux) em espaco de usuario, aqui na libc:
 *   - modo canonico (cooked): buffer de linha, echo, erase (VERASE), kill
 *     (VKILL), fim por \n / VEOF; entrega uma linha por read();
 *   - modo raw: passthrough (o editor do proprio ash cuida da edicao);
 *   - VINTR->SIGINT, VQUIT->SIGQUIT; mapeamento ICRNL/INLCR/IGNCR; ONLCR na
 *     saida (\n -> \r\n).
 * termios e winsize vivem em memoria (um pty por processo — o tty controlador).
 * O transporte sao os pipes que o master (dispd) passou como fd 0/1/2.
 *
 * Limite conhecido: VINTR so gera sinal quando alguem esta LENDO o tty (como no
 * caso de um programa em read cooked). Interromper um comando que roda em
 * segundo plano (o shell esperando) exige grupo de processo + sinal entre
 * processos (job control) — fica p/ depois.
 */
#include "nt/ntpriv.h"

/* ---- termios (um pty por processo) ---- */
static struct nt_termios g_tio;
static int g_tio_ready;

static void tio_ensure(void)
{
    if (g_tio_ready)
        return;
    nt_memset(&g_tio, 0, sizeof g_tio);
    g_tio.c_iflag = NT_ICRNL | NT_IXON;
    g_tio.c_oflag = NT_OPOST | NT_ONLCR;
    g_tio.c_cflag = NT_CS8 | NT_CREAD | NT_CLOCAL;
    g_tio.c_lflag = NT_ISIG | NT_ICANON | NT_ECHO | NT_ECHOE | NT_ECHOK |
                    NT_ECHOCTL | NT_IEXTEN;
    g_tio.c_cc[NT_VINTR]  = 3;     /* ^C */
    g_tio.c_cc[NT_VQUIT]  = 28;    /* ^\ */
    g_tio.c_cc[NT_VERASE] = 0x7f;  /* DEL */
    g_tio.c_cc[NT_VKILL]  = 21;    /* ^U */
    g_tio.c_cc[NT_VEOF]   = 4;     /* ^D */
    g_tio.c_cc[NT_VMIN]   = 1;
    g_tio.c_cc[NT_VTIME]  = 0;
    g_tio_ready = 1;
}

void nt_pty_tcget(struct nt_termios *tio)       { tio_ensure(); *tio = g_tio; }
void nt_pty_tcset(const struct nt_termios *tio) { tio_ensure(); g_tio = *tio; }
int  nt_pty_onlcr(void)
{
    tio_ensure();
    return (g_tio.c_oflag & NT_OPOST) && (g_tio.c_oflag & NT_ONLCR);
}

/* ---- winsize via file-mapping compartilhado com o master ---- */
static volatile unsigned short *g_ws;

void nt_pty_winsize(unsigned short *cols, unsigned short *rows)
{
    if (!g_ws) {
        char name[64];
        if (GetEnvironmentVariableA("NTU_PTY_WS", name, sizeof name) > 0) {
            HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
            if (m) {
                g_ws = (volatile unsigned short *)MapViewOfFile(m, FILE_MAP_READ, 0, 0, 4);
                CloseHandle(m);   /* audit #112: a view sobrevive ao close do handle */
            }
        }
    }
    unsigned short c = g_ws ? g_ws[0] : 0;
    unsigned short r = g_ws ? g_ws[1] : 0;
    *cols = c ? c : 80;
    *rows = r ? r : 24;
}

/* ---- saida (OPOST): \n -> \r\n ---- */
nt_sc_t nt_pty_write(struct nt_fd *slot, const void *buf, uint64_t count)
{
    DWORD amount = count > 0x7ffff000ULL ? 0x7ffff000UL : (DWORD)count;
    DWORD w;
    tio_ensure();
    if (!(g_tio.c_oflag & NT_OPOST) || !(g_tio.c_oflag & NT_ONLCR)) {
        if (!WriteFile(slot->handle, buf, amount, &w, 0))
            return nt_last_error();
        return (nt_sc_t)w;
    }
    const unsigned char *b = (const unsigned char *)buf;
    DWORD i, start = 0;
    for (i = 0; i < amount; i++) {
        if (b[i] != '\n')
            continue;
        if (i > start && !WriteFile(slot->handle, b + start, i - start, &w, 0))
            return nt_last_error();
        if (!WriteFile(slot->handle, "\r\n", 2, &w, 0))
            return nt_last_error();
        start = i + 1;
    }
    if (amount > start && !WriteFile(slot->handle, b + start, amount - start, &w, 0))
        return nt_last_error();
    return (nt_sc_t)amount;   /* conta os bytes logicos, sem os \r injetados */
}

/* echo vai pro lado de SAIDA do pty (fd 1) com OPOST */
static void pty_echo(const void *b, int n)
{
    struct nt_fd *out = nt_fd_get(1);
    if (!out || out->kind != NT_FD_PTY)
        out = nt_fd_get(2);
    if (out && out->kind == NT_FD_PTY)
        nt_pty_write(out, b, (uint64_t)n);
}

static void pty_echo_char(unsigned char c)
{
    if (c < 0x20 && c != '\n' && c != '\t' && (g_tio.c_lflag & NT_ECHOCTL)) {
        unsigned char s[2] = { '^', (unsigned char)(c + 0x40) };
        pty_echo(s, 2);
    } else {
        pty_echo(&c, 1);
    }
}

/* ---- leitura raw (passthrough) ---- */
static nt_sc_t pty_read_raw(struct nt_fd *slot, void *buf, DWORD amount)
{
    DWORD got = 0;
    if (slot->flags & NT_O_NONBLOCK) {
        DWORD avail = 0;
        if (PeekNamedPipe(slot->handle, 0, 0, 0, &avail, 0)) {
            if (!avail) return -NT_EAGAIN;
        } else if (GetLastError() == ERROR_BROKEN_PIPE) {
            return 0;
        }
    }
    if (!ReadFile(slot->handle, buf, amount, &got, 0)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) return 0;
        if (e == ERROR_OPERATION_ABORTED) return -NT_EINTR;   /* Ctrl-C acordou */
        return nt_error(e);
    }
    return (nt_sc_t)got;
}

/* ---- leitura cooked (canonica): monta uma linha ---- */
static unsigned char g_line[4096];
static int g_line_len;    /* bytes na linha */
static int g_line_out;    /* ja entregue ao leitor (entrega parcial) */
static int g_line_ready;  /* linha completa (\n ou VEOF) */

nt_sc_t nt_pty_read(struct nt_fd *slot, void *ubuf, uint64_t count)
{
    tio_ensure();
    DWORD amount = count > 0x7ffff000ULL ? 0x7ffff000UL : (DWORD)count;
    if (amount == 0)
        return 0;

    if (!(g_tio.c_lflag & NT_ICANON))
        return pty_read_raw(slot, ubuf, amount);

    while (!g_line_ready) {
        unsigned char c;
        DWORD got = 0;
        if (slot->flags & NT_O_NONBLOCK) {   /* audit #105: canonico tambem respeita
                                              * O_NONBLOCK — sem bytes e sem linha
                                              * completa -> EAGAIN (nao bloqueia) */
            DWORD avail = 0;
            if (PeekNamedPipe(slot->handle, 0, 0, 0, &avail, 0)) {
                if (!avail)
                    return -NT_EAGAIN;
            } else if (GetLastError() == ERROR_BROKEN_PIPE) {
                g_line_ready = 1;
                break;
            }
        }
        if (!ReadFile(slot->handle, &c, 1, &got, 0)) {
            DWORD e = GetLastError();
            if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF) { g_line_ready = 1; break; }
            if (e == ERROR_OPERATION_ABORTED) return -NT_EINTR;
            return nt_error(e);
        }
        if (got == 0) { g_line_ready = 1; break; }   /* EOF */

        /* mapeamento de entrada */
        if (c == '\r') {
            if (g_tio.c_iflag & NT_IGNCR) continue;
            if (g_tio.c_iflag & NT_ICRNL) c = '\n';
        } else if (c == '\n') {
            if (g_tio.c_iflag & NT_INLCR) c = '\r';
        }

        /* sinais (VINTR/VQUIT) */
        if (g_tio.c_lflag & NT_ISIG) {
            if (c == g_tio.c_cc[NT_VINTR]) {
                if (g_tio.c_lflag & NT_ECHOCTL) pty_echo("^C", 2);
                pty_echo("\n", 1);
                g_line_len = g_line_out = 0;
                nt_signal_post_local(2);      /* SIGINT */
                return -NT_EINTR;
            }
            if (c == g_tio.c_cc[NT_VQUIT]) {
                if (g_tio.c_lflag & NT_ECHOCTL) pty_echo("^\\", 2);
                pty_echo("\n", 1);
                g_line_len = g_line_out = 0;
                nt_signal_post_local(3);      /* SIGQUIT */
                return -NT_EINTR;
            }
        }

        /* erase / kill */
        if (c == g_tio.c_cc[NT_VERASE] || c == '\b') {
            if (g_line_len > 0) {
                g_line_len--;
                if ((g_tio.c_lflag & NT_ECHO) && (g_tio.c_lflag & NT_ECHOE))
                    pty_echo("\b \b", 3);
            }
            continue;
        }
        if (c == g_tio.c_cc[NT_VKILL]) {
            if (g_tio.c_lflag & NT_ECHO)
                while (g_line_len > 0) { g_line_len--; pty_echo("\b \b", 3); }
            else
                g_line_len = 0;
            continue;
        }
        if (c == g_tio.c_cc[NT_VEOF]) {   /* ^D: entrega o que tem (ou 0 = EOF) */
            g_line_ready = 1;
            break;
        }

        /* char normal */
        if (g_line_len < (int)sizeof g_line - 1) {
            g_line[g_line_len++] = c;
            if (g_tio.c_lflag & NT_ECHO) pty_echo_char(c);
        }
        if (c == '\n') { g_line_ready = 1; break; }
    }

    /* entrega (parcial se o buffer do leitor for menor que a linha) */
    int avail = g_line_len - g_line_out;
    DWORD n = (DWORD)avail < amount ? (DWORD)avail : amount;
    if (n > 0)
        nt_memcpy(ubuf, g_line + g_line_out, (size_t)n);
    g_line_out += (int)n;
    if (g_line_out >= g_line_len) { g_line_len = g_line_out = 0; g_line_ready = 0; }
    return (nt_sc_t)n;
}
