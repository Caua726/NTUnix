/*
 * term_scrape.c — fallback quando o ConPTY nao existe (WinPE fino).
 *
 * Padrao winpty (rprichard/winpty): dono de um console oculto, le a grade via
 * ReadConsoleOutputA por bandas e injeta teclas via WriteConsoleInputA. Como
 * um processo so pode estar preso a UM console, este fallback suporta UM unico
 * terminal (o primeiro); os demais falham limpo. O ConPTY e' o caminho normal.
 *
 * Referencias reais: winpty (agente que compartilha o console do filho),
 * MS "reading-and-writing-blocks-of-characters-and-attributes".
 */
#include "term.h"

static volatile LONG g_console_used;

typedef struct {
    HANDLE conout;   /* CONOUT$ */
    HANDLE conin;    /* CONIN$  */
    HANDLE hproc;
    HANDLE reader;
    volatile LONG stop;
} Scrape;

/* atributo do console -> indice ANSI 0..15 (Win usa R=4,G=2,B=1) */
static unsigned char win_to_ansi(WORD a, int bg)
{
    WORD r = bg ? BACKGROUND_RED : FOREGROUND_RED;
    WORD g = bg ? BACKGROUND_GREEN : FOREGROUND_GREEN;
    WORD b = bg ? BACKGROUND_BLUE : FOREGROUND_BLUE;
    WORD hi = bg ? BACKGROUND_INTENSITY : FOREGROUND_INTENSITY;
    unsigned char idx = 0;
    if (a & r) idx |= 1;
    if (a & g) idx |= 2;
    if (a & b) idx |= 4;
    if (a & hi) idx |= 8;
    return idx;
}

static void scrape_frame(Terminal *t, Scrape *s)
{
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(s->conout, &csbi))
        return;
    int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    if (cols < 1 || rows < 1)
        return;
    if (cols > t->cols) cols = t->cols;
    if (rows > t->rows) rows = t->rows;

    /* le por bandas (limite de ~64KB/chamada do ReadConsoleOutput) */
    static CHAR_INFO band[4096];
    int band_rows = (int)(sizeof band / sizeof band[0]) / (cols > 0 ? cols : 1);
    if (band_rows < 1) band_rows = 1;

    EnterCriticalSection(&t->lock);
    for (int y0 = 0; y0 < rows; y0 += band_rows) {
        int bh = rows - y0 < band_rows ? rows - y0 : band_rows;
        COORD bsize = { (SHORT)cols, (SHORT)bh };
        COORD bcoord = { 0, 0 };
        SMALL_RECT rd = {
            (SHORT)csbi.srWindow.Left,
            (SHORT)(csbi.srWindow.Top + y0),
            (SHORT)(csbi.srWindow.Left + cols - 1),
            (SHORT)(csbi.srWindow.Top + y0 + bh - 1)
        };
        if (!ReadConsoleOutputA(s->conout, band, bsize, bcoord, &rd))
            continue;
        for (int y = 0; y < bh; y++) {
            for (int x = 0; x < cols; x++) {
                CHAR_INFO *ci = &band[y * cols + x];
                Cell *c = &t->grid[(y0 + y) * t->cols + x];
                unsigned char ch = (unsigned char)ci->Char.AsciiChar;
                c->ch = ch ? ch : ' ';
                c->fg = win_to_ansi(ci->Attributes, 0);
                c->bg = win_to_ansi(ci->Attributes, 1);
                c->attr = 0;
            }
        }
    }
    t->cur_x = csbi.dwCursorPosition.X - csbi.srWindow.Left;
    t->cur_y = csbi.dwCursorPosition.Y - csbi.srWindow.Top;
    t->dirty = 1;
    LeaveCriticalSection(&t->lock);
}

static DWORD WINAPI reader_main(LPVOID arg)
{
    Terminal *t = (Terminal *)arg;
    Scrape *s = (Scrape *)t->impl;
    while (!s->stop) {
        scrape_frame(t, s);
        if (WaitForSingleObject(s->hproc, 33) == WAIT_OBJECT_0)
            break;
    }
    scrape_frame(t, s);   /* ultimo quadro */
    t->alive = 0;
    return 0;
}

static int scrape_start(Terminal *t, const char *cmdline, int cols, int rows)
{
    if (InterlockedCompareExchange(&g_console_used, 1, 0) != 0)
        return -1;   /* ja tem um scrape usando o console */

    Scrape *s = (Scrape *)calloc(1, sizeof *s);
    if (!s) {
        InterlockedExchange(&g_console_used, 0);
        return -1;
    }
    t->impl = s;

    if (!AllocConsole()) {
        /* talvez ja tenhamos um console; segue tentando abrir CONOUT$ */
    }
    HWND cw = GetConsoleWindow();
    if (cw)
        ShowWindow(cw, SW_HIDE);

    /* dimensiona o buffer/janela do console */
    COORD bs = { (SHORT)(cols > 0 ? cols : 80), (SHORT)(rows > 0 ? rows : 24) };
    SetConsoleScreenBufferSize(GetStdHandle(STD_OUTPUT_HANDLE), bs);
    SMALL_RECT wr = { 0, 0, (SHORT)(bs.X - 1), (SHORT)(bs.Y - 1) };
    SetConsoleWindowInfo(GetStdHandle(STD_OUTPUT_HANDLE), TRUE, &wr);

    s->conout = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                            OPEN_EXISTING, 0, NULL);
    s->conin = CreateFileA("CONIN$", GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, 0, NULL);
    if (s->conout == INVALID_HANDLE_VALUE)
        goto fail;

    /* filho herda o nosso console (sem CREATE_NEW_CONSOLE) */
    STARTUPINFOA si;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    char cmd[1024];
    strncpy(cmd, cmdline ? cmdline : "cmd.exe", sizeof cmd - 1);
    cmd[sizeof cmd - 1] = 0;
    PROCESS_INFORMATION pi;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, ntu_root(),
                        &si, &pi))
        goto fail;
    CloseHandle(pi.hThread);
    s->hproc = pi.hProcess;
    t->pid = pi.dwProcessId;
    s->reader = CreateThread(NULL, 0, reader_main, t, 0, NULL);
    if (!s->reader) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hProcess);
        s->hproc = NULL;
        goto fail;
    }
    return 0;

fail:
    if (s->conout && s->conout != INVALID_HANDLE_VALUE) CloseHandle(s->conout);
    if (s->conin && s->conin != INVALID_HANDLE_VALUE)   CloseHandle(s->conin);
    free(s);
    t->impl = NULL;
    InterlockedExchange(&g_console_used, 0);
    return -1;
}

static void scrape_input(Terminal *t, const char *bytes, int n)
{
    Scrape *s = (Scrape *)t->impl;
    if (!s || s->conin == INVALID_HANDLE_VALUE)
        return;
    for (int i = 0; i < n; i++) {
        INPUT_RECORD ir[2];
        ZeroMemory(ir, sizeof ir);
        SHORT vks = VkKeyScanA(bytes[i]);
        WORD vk = (WORD)(vks & 0xff);
        for (int k = 0; k < 2; k++) {
            ir[k].EventType = KEY_EVENT;
            ir[k].Event.KeyEvent.bKeyDown = (k == 0);
            ir[k].Event.KeyEvent.wRepeatCount = 1;
            ir[k].Event.KeyEvent.wVirtualKeyCode = vk;
            ir[k].Event.KeyEvent.wVirtualScanCode =
                (WORD)MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
            ir[k].Event.KeyEvent.uChar.AsciiChar = bytes[i];
        }
        DWORD wr;
        WriteConsoleInputA(s->conin, ir, 2, &wr);
    }
}

static void scrape_resize(Terminal *t, int cols, int rows)
{
    Scrape *s = (Scrape *)t->impl;
    if (!s)
        return;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    /* encolher: janela ANTES do buffer (Windows exige janela <= buffer) — #64 */
    SMALL_RECT minw = { 0, 0, 0, 0 };
    SetConsoleWindowInfo(s->conout, TRUE, &minw);
    COORD bs = { (SHORT)cols, (SHORT)rows };
    SetConsoleScreenBufferSize(s->conout, bs);
    SMALL_RECT wr = { 0, 0, (SHORT)(cols - 1), (SHORT)(rows - 1) };
    SetConsoleWindowInfo(s->conout, TRUE, &wr);
}

static void scrape_close(Terminal *t)
{
    Scrape *s = (Scrape *)t->impl;
    if (!s)
        return;
    InterlockedExchange(&s->stop, 1);
    if (s->hproc)
        TerminateProcess(s->hproc, 0);
    if (s->reader) {
        WaitForSingleObject(s->reader, 2000);
        CloseHandle(s->reader);
    }
    if (s->conout && s->conout != INVALID_HANDLE_VALUE) CloseHandle(s->conout);
    if (s->conin && s->conin != INVALID_HANDLE_VALUE)   CloseHandle(s->conin);
    if (s->hproc) CloseHandle(s->hproc);
    FreeConsole();
    free(s);
    t->impl = NULL;
    InterlockedExchange(&g_console_used, 0);
}

TerminalBackend term_scrape_backend = {
    "scrape", scrape_start, scrape_input, scrape_resize, scrape_close
};
