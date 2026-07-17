/*
 * ntclock.c — app grafico de demonstracao (relogio) da fronteira apps<->dispd.
 *
 * Processo separado: conecta no dispd, pede uma superficie, abre a MESMA
 * section (memoria compartilhada), desenha com GDI e manda APP-COMMIT a cada
 * segundo. Prova o M4 (apps GUI nativos, tilados pelo ntwm junto dos terminais).
 *
 * Rode de um terminal ash: `ntclock.exe &`
 */
#include "../../common/ntu.h"

static HANDLE connect_app(void)
{
    for (int i = 0; i < 100; i++) {
        HANDLE h = CreateFileA(NTU_PIPE_DISPD_APP, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeA(NTU_PIPE_DISPD_APP, 2000);
            continue;
        }
        Sleep(200);   /* dispd talvez ainda subindo */
    }
    return INVALID_HANDLE_VALUE;
}

int main(void)
{
    HANDLE pipe = connect_app();
    if (pipe == INVALID_HANDLE_VALUE)
        return 1;
    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(pipe, &mode, NULL, NULL);

    int W = 340, H = 150;
    char hello[64];
    int hn = snprintf(hello, sizeof hello, "APP-HELLO %d %d relogio", W, H);
    DWORD wr;
    WriteFile(pipe, hello, (DWORD)hn, &wr, NULL);

    char buf[128];
    DWORD n = 0;
    if (!ReadFile(pipe, buf, sizeof buf - 1, &n, NULL) || n == 0)
        return 1;
    buf[n] = 0;

    char name[64] = "";
    if (sscanf(buf, "APP-SURFACE %63s %d %d", name, &W, &H) < 1)
        return 1;

    HANDLE sec = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!sec)
        return 1;

    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = W;
    bmi.bmiHeader.biHeight = -H;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, sec, 0);
    if (!dib)
        return 1;
    SelectObject(dc, dib);

    HFONT big = CreateFontA(56, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET,
                            0, 0, 0, FIXED_PITCH, NULL);
    HFONT small = CreateFontA(18, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                              0, 0, 0, DEFAULT_PITCH, NULL);

    for (;;) {
        RECT full = { 0, 0, W, H };
        HBRUSH bg = CreateSolidBrush(RGB(20, 22, 30));
        FillRect(dc, &full, bg);
        DeleteObject(bg);

        SYSTEMTIME st;
        GetLocalTime(&st);
        char t[32];
        snprintf(t, sizeof t, "%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        SetBkMode(dc, TRANSPARENT);
        SelectObject(dc, big);
        SetTextColor(dc, RGB(230, 230, 240));
        RECT r1 = { 0, 12, W, H - 32 };
        DrawTextA(dc, t, -1, &r1, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        char d[32];
        snprintf(d, sizeof d, "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        SelectObject(dc, small);
        SetTextColor(dc, RGB(150, 150, 170));
        RECT r2 = { 0, H - 30, W, H };
        DrawTextA(dc, d, -1, &r2, DT_CENTER | DT_SINGLELINE);

        GdiFlush();
        if (!WriteFile(pipe, "APP-COMMIT", 10, &wr, NULL))
            break;   /* dispd saiu */
        Sleep(1000);
    }
    return 0;
}
