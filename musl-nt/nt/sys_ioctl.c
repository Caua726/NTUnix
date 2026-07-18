#include "nt/ntpriv.h"

#define NT_TIOCSWINSZ 0x5414UL
#define NT_FIONREAD   0x541BUL

/* termios via console da MS: as flags do termios mapeiam nos modos de console.
 * TCGETS/TCSETS(+act) são o que tcgetattr/tcsetattr da musl emitem. */
#define NT_TCGETS   0x5401UL
#define NT_TCSETS   0x5402UL
#define NT_TCSETSW  0x5403UL
#define NT_TCSETSF  0x5404UL

/* struct termios do Linux x86_64 (arch generic) — mesmo layout que a musl usa */
struct nt_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

/* c_lflag (x86_64 generic) */
#define NT_ISIG   0000001u
#define NT_ICANON 0000002u
#define NT_ECHO   0000010u
#define NT_IEXTEN 0100000u
/* c_iflag */
#define NT_ICRNL  0000400u
#define NT_IXON   0002000u
/* c_oflag */
#define NT_OPOST  0000001u
#define NT_ONLCR  0000004u
/* c_cflag */
#define NT_CS8    0000060u
#define NT_CREAD  0000200u
#define NT_CLOCAL 0004000u
/* índices em c_cc (x86_64 generic) */
#define NT_VINTR  0
#define NT_VERASE 2
#define NT_VEOF   4
#define NT_VTIME  5
#define NT_VMIN   6

/* ---- pty slave (sem ConPTY): termios em memoria + winsize via file-mapping
 * compartilhado com o master (dispd). Um pty por processo (o tty controlador). */
static struct nt_termios g_pty_tio;
static int g_pty_tio_ready;
static volatile unsigned short *g_pty_ws;   /* view do mapping: [0]=cols [1]=rows */

static void pty_tio_ensure(void)
{
    if (g_pty_tio_ready)
        return;
    nt_memset(&g_pty_tio, 0, sizeof g_pty_tio);
    g_pty_tio.c_iflag = NT_ICRNL | NT_IXON;
    g_pty_tio.c_oflag = NT_OPOST | NT_ONLCR;
    g_pty_tio.c_cflag = NT_CS8 | NT_CREAD | NT_CLOCAL;
    g_pty_tio.c_lflag = NT_ISIG | NT_ICANON | NT_ECHO | NT_IEXTEN;
    g_pty_tio.c_cc[NT_VINTR]  = 3;
    g_pty_tio.c_cc[NT_VEOF]   = 4;
    g_pty_tio.c_cc[NT_VERASE] = 0x7f;
    g_pty_tio.c_cc[NT_VMIN]   = 1;
    g_pty_tio.c_cc[NT_VTIME]  = 0;
    g_pty_tio_ready = 1;
}

static void pty_winsize(unsigned short *cols, unsigned short *rows)
{
    if (!g_pty_ws) {
        char name[64];
        if (GetEnvironmentVariableA("NTU_PTY_WS", name, sizeof name) > 0) {
            HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
            if (m)   /* a view sobrevive ao close; mantemos pela vida do processo */
                g_pty_ws = (volatile unsigned short *)
                    MapViewOfFile(m, FILE_MAP_READ, 0, 0, 4);
        }
    }
    unsigned short c = g_pty_ws ? g_pty_ws[0] : 0;
    unsigned short r = g_pty_ws ? g_pty_ws[1] : 0;
    *cols = c ? c : 80;
    *rows = r ? r : 24;
}

int nt_pty_onlcr(void)
{
    pty_tio_ensure();
    return (g_pty_tio.c_oflag & NT_OPOST) && (g_pty_tio.c_oflag & NT_ONLCR);
}

static nt_sc_t termios_get(struct nt_fd *slot, struct nt_termios *tio)
{
    DWORD mode;
    if (slot->kind == NT_FD_PTY) {           /* pty: estado em memoria */
        pty_tio_ensure();
        *tio = g_pty_tio;
        return 0;
    }
    if (slot->kind != NT_FD_CONSOLE || !GetConsoleMode(slot->handle, &mode))
        return -NT_ENOTTY;
    nt_memset(tio, 0, sizeof *tio);
    /* line discipline: derivado do estado real do console */
    if (mode & ENABLE_LINE_INPUT)      tio->c_lflag |= NT_ICANON;
    if (mode & ENABLE_ECHO_INPUT)      tio->c_lflag |= NT_ECHO;
    if (mode & ENABLE_PROCESSED_INPUT) tio->c_lflag |= NT_ISIG;
    tio->c_lflag |= NT_IEXTEN;
    /* defaults sensatos de um tty pros bits que o console não expõe */
    tio->c_iflag = NT_ICRNL | NT_IXON;
    tio->c_oflag = NT_OPOST | NT_ONLCR;
    tio->c_cflag = NT_CS8 | NT_CREAD | NT_CLOCAL;
    tio->c_cc[NT_VINTR]  = 3;    /* ^C */
    tio->c_cc[NT_VEOF]   = 4;    /* ^D */
    tio->c_cc[NT_VERASE] = 0x7f; /* DEL */
    tio->c_cc[NT_VMIN]   = 1;
    tio->c_cc[NT_VTIME]  = 0;
    return 0;
}

static nt_sc_t termios_set(struct nt_fd *slot, const struct nt_termios *tio)
{
    DWORD mode;
    if (slot->kind == NT_FD_PTY) {           /* pty: guarda o estado em memoria */
        pty_tio_ensure();
        g_pty_tio = *tio;
        return 0;
    }
    if (slot->kind != NT_FD_CONSOLE || !GetConsoleMode(slot->handle, &mode))
        return -NT_ENOTTY;
    /* preserva os bits que não gerencio; reaplica a disciplina de linha */
    mode &= ~(DWORD)(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT |
                     ENABLE_VIRTUAL_TERMINAL_INPUT);
    if (tio->c_lflag & NT_ICANON) {
        mode |= ENABLE_LINE_INPUT;
    } else {
        /* raw (ex.: line-editing do shell): a entrada VT entrega as setas e
         * outras teclas como sequências ESC, que é o que o editor lê. */
        mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    }
    if (tio->c_lflag & NT_ECHO)   mode |= ENABLE_ECHO_INPUT;
    if (tio->c_lflag & NT_ISIG)   mode |= ENABLE_PROCESSED_INPUT;
    if (!SetConsoleMode(slot->handle, mode))
        return nt_last_error();
    return 0;
}

nt_sc_t nt_sys_ioctl(nt_sc_t fd, nt_sc_t request, nt_sc_t arg)
{
    struct nt_fd *slot = nt_fd_get((int)fd);
    if (!slot) return -NT_EBADF;
    if (request == NT_TIOCGWINSZ) {
        struct nt_winsize *winsize = (void *)(uintptr_t)arg;
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!winsize) return -NT_EFAULT;
        if (slot->kind == NT_FD_PTY) {       /* pty: winsize do mapping do master */
            unsigned short c, r;
            pty_winsize(&c, &r);
            winsize->ws_col = c;
            winsize->ws_row = r;
            winsize->ws_xpixel = 0;
            winsize->ws_ypixel = 0;
            return 0;                         /* sucesso -> isatty() = true */
        }
        if (slot->kind != NT_FD_CONSOLE ||
            !GetConsoleScreenBufferInfo(slot->handle, &info))
            return -NT_ENOTTY;
        winsize->ws_col = (uint16_t)(info.srWindow.Right - info.srWindow.Left + 1);
        winsize->ws_row = (uint16_t)(info.srWindow.Bottom - info.srWindow.Top + 1);
        winsize->ws_xpixel = 0;
        winsize->ws_ypixel = 0;
        return 0;
    }
    if (request == NT_TIOCSWINSZ)
        return (slot->kind == NT_FD_CONSOLE || slot->kind == NT_FD_PTY)
                   ? 0 : -NT_ENOTTY;
    if (request == NT_TCGETS) {
        struct nt_termios *tio = (void *)(uintptr_t)arg;
        if (!tio) return -NT_EFAULT;
        return termios_get(slot, tio);
    }
    if (request == NT_TCSETS || request == NT_TCSETSW || request == NT_TCSETSF) {
        const struct nt_termios *tio = (const void *)(uintptr_t)arg;
        if (!tio) return -NT_EFAULT;
        return termios_set(slot, tio);
    }
    if (request == NT_FIONREAD) {
        int *available_out = (void *)(uintptr_t)arg;
        DWORD available = 0;
        if (!available_out) return -NT_EFAULT;
        if (slot->kind == NT_FD_PIPE || slot->kind == NT_FD_PTY) {
            if (!PeekNamedPipe(slot->handle, 0, 0, 0, &available, 0))
                return nt_last_error();
        } else if (slot->kind == NT_FD_CONSOLE) {
            if (!GetNumberOfConsoleInputEvents(slot->handle, &available))
                return nt_last_error();
        } else {
            return -NT_ENOTTY;
        }
        *available_out = (int)available;
        return 0;
    }
    return -NT_ENOTTY;
}
