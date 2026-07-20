/*
 * ntbar - barra independente do desktop NTUnix.
 *
 * E uma app protocol v2 com role=layer, anchors top+left+right e zona
 * exclusiva de 34 px. Politica continua no ntwm: cliques usam ntwmctl e o
 * estado chega pelo stream de eventos.
 */
#include "../../common/ntu.h"
#include "../../common/ntuapp.h"
#include "../../common/ntuwm.h"

typedef struct Surface {
    HANDLE section;
    HDC dc;
    HBITMAP dib;
    void *bits;
    int w, h;
    unsigned serial;
} Surface;

static Surface g_surface;
static HANDLE g_app = INVALID_HANDLE_VALUE;
static HANDLE g_events = INVALID_HANDLE_VALUE;
static int g_active = 1;
static int g_occupied[NTUWM_WS];
static int g_urgent[NTUWM_WS];
static char g_names[NTUWM_WS][32];
static char g_layout[32] = "dwindle";
static char g_title[256] = "NTUnix";
static int g_ws_x0[NTUWM_WS], g_ws_x1[NTUWM_WS];
static int g_layout_x0, g_layout_x1;

/* O ativo existe mesmo vazio; os demais so aparecem enquanto possuem
 * clientes (ou urgencia). Um workspace vazio e inativo some da barra, mas
 * continua acessivel/criavel pelos dispatchers numericos. */
static int workspace_visible(int index)
{
    return index >= 0 && index < NTUWM_WS &&
           (index + 1 == g_active || g_occupied[index] || g_urgent[index]);
}

static int next_visible_workspace(int active, int direction)
{
    int start = active - 1;
    if (start < 0 || start >= NTUWM_WS)
        start = 0;
    for (int step = 1; step <= NTUWM_WS; step++) {
        int i = (start + direction * step) % NTUWM_WS;
        if (i < 0) i += NTUWM_WS;
        if (workspace_visible(i))
            return i + 1;
    }
    return active;
}

static HANDLE connect_pipe(const char *name, DWORD access)
{
    for (int i = 0; i < 100; i++) {
        HANDLE h = CreateFileA(name, access, 0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, NULL, NULL);
            return h;
        }
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeA(name, 500);
        else
            Sleep(100);
    }
    return INVALID_HANDLE_VALUE;
}

static int write_msg(HANDLE pipe, const char *s)
{
    DWORD n = 0;
    return WriteFile(pipe, s, (DWORD)strlen(s), &n, NULL) &&
           n == (DWORD)strlen(s);
}

static int read_msg(HANDLE pipe, char *buf, int cap, int wait)
{
    DWORD available = 0;
    if (!wait) {
        if (!PeekNamedPipe(pipe, NULL, 0, NULL, &available, NULL))
            return -1;
        if (!available)
            return 0;
    }
    DWORD n = 0;
    if (!ReadFile(pipe, buf, (DWORD)(cap - 1), &n, NULL) || !n)
        return -1;
    buf[n] = 0;
    return (int)n;
}

static int ctl(const char *command)
{
    HANDLE p = connect_pipe(NTU_PIPE_NTWM_CTL, GENERIC_READ | GENERIC_WRITE);
    if (p == INVALID_HANDLE_VALUE)
        return 0;
    char reply[256];
    DWORD n = 0;
    int ok = write_msg(p, command) &&
             ReadFile(p, reply, sizeof reply - 1, &n, NULL) && n > 0;
    CloseHandle(p);
    return ok;
}

static void surface_drop(void)
{
    if (g_surface.dc) DeleteDC(g_surface.dc);
    if (g_surface.dib) DeleteObject(g_surface.dib);
    if (g_surface.section) CloseHandle(g_surface.section);
    ZeroMemory(&g_surface, sizeof g_surface);
}

static int surface_configure(unsigned serial, const char *name, int w, int h)
{
    HANDLE section = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
    if (!section)
        return 0;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof bmi);
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP dib = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS,
                                        &bits, section, 0) : NULL;
    if (!dc || !dib) {
        if (dc) DeleteDC(dc);
        CloseHandle(section);
        return 0;
    }
    SelectObject(dc, dib);
    surface_drop();
    g_surface.section = section;
    g_surface.dc = dc;
    g_surface.dib = dib;
    g_surface.bits = bits;
    g_surface.w = w;
    g_surface.h = h;
    g_surface.serial = serial;
    char ack[48];
    snprintf(ack, sizeof ack, "%s %u", APP_CMD_ACK, serial);
    return write_msg(g_app, ack);
}

static void draw_utf8(HDC dc, const char *text, RECT *r, UINT flags)
{
    WCHAR wide[512];
    int n = MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 512);
    if (n > 0)
        DrawTextW(dc, wide, n - 1, r, flags);
}

static void alpha_opaque(void)
{
    unsigned char *p = (unsigned char *)g_surface.bits;
    size_t pixels = (size_t)g_surface.w * g_surface.h;
    for (size_t i = 0; i < pixels; i++)
        p[i * 4 + 3] = 255;
}

static void render(void)
{
    if (!g_surface.dc || g_surface.w < 1)
        return;
    HDC dc = g_surface.dc;
    RECT full = { 0, 0, g_surface.w, g_surface.h };
    HBRUSH bg = CreateSolidBrush(RGB(17, 20, 36));
    FillRect(dc, &full, bg);
    DeleteObject(bg);

    HFONT font = CreateFontA(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                             DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, "Segoe UI");
    HFONT oldfont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    int x = 8;
    for (int i = 0; i < NTUWM_WS; i++)
        g_ws_x0[i] = g_ws_x1[i] = -1;
    for (int i = 0; i < NTUWM_WS; i++) {
        if (!workspace_visible(i))
            continue;
        char label[32];
        memcpy(label, g_names[i], sizeof label);
        label[sizeof label - 1] = 0;
        if (!label[0]) snprintf(label, sizeof label, "%d", i + 1);
        SIZE sz;
        GetTextExtentPoint32A(dc, label, (int)strlen(label), &sz);
        int pw = sz.cx + 18;
        if (pw < 30) pw = 30;
        RECT pill = { x, 5, x + pw, g_surface.h - 5 };
        COLORREF pc = i + 1 == g_active ? RGB(122, 162, 247) :
                      g_urgent[i] ? RGB(247, 118, 142) :
                      g_occupied[i] ? RGB(45, 52, 78) : RGB(25, 29, 49);
        HBRUSH pb = CreateSolidBrush(pc);
        HPEN pp = CreatePen(PS_SOLID, 1, pc);
        HBRUSH opb = (HBRUSH)SelectObject(dc, pb);
        HPEN opp = (HPEN)SelectObject(dc, pp);
        RoundRect(dc, pill.left, pill.top, pill.right, pill.bottom, 14, 14);
        SelectObject(dc, opb); SelectObject(dc, opp);
        DeleteObject(pb); DeleteObject(pp);
        SetTextColor(dc, i + 1 == g_active ? RGB(11, 15, 28)
                                           : RGB(205, 214, 244));
        draw_utf8(dc, label, &pill, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (g_occupied[i] && i + 1 != g_active) {
            RECT dot = { pill.left + pw / 2 - 2, pill.bottom - 5,
                         pill.left + pw / 2 + 2, pill.bottom - 1 };
            HBRUSH db = CreateSolidBrush(RGB(122, 162, 247));
            FillRect(dc, &dot, db);
            DeleteObject(db);
        }
        g_ws_x0[i] = x;
        g_ws_x1[i] = x + pw;
        x += pw + 5;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    char clock[32];
    snprintf(clock, sizeof clock, "%02d:%02d", st.wHour, st.wMinute);
    SIZE csz;
    GetTextExtentPoint32A(dc, clock, (int)strlen(clock), &csz);
    RECT cr = { g_surface.w - csz.cx - 16, 0, g_surface.w - 10, g_surface.h };
    SetTextColor(dc, RGB(205, 214, 244));
    DrawTextA(dc, clock, -1, &cr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    char center[360];
    snprintf(center, sizeof center, "%s  -  %s", g_layout,
             g_title[0] ? g_title : "desktop");
    int center_left = g_surface.w / 4;
    int center_right = g_surface.w * 3 / 4;
    RECT tr = { center_left, 0, center_right, g_surface.h };
    SetTextColor(dc, RGB(169, 177, 214));
    draw_utf8(dc, center, &tr,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SIZE lsz;
    GetTextExtentPoint32A(dc, g_layout, (int)strlen(g_layout), &lsz);
    g_layout_x0 = g_surface.w / 2 - lsz.cx / 2 - 8;
    g_layout_x1 = g_surface.w / 2 + lsz.cx / 2 + 8;

    SelectObject(dc, oldfont);
    DeleteObject(font);
    GdiFlush();
    alpha_opaque();
    char commit[48];
    snprintf(commit, sizeof commit, "%s %u", APP_CMD_COMMIT, g_surface.serial);
    write_msg(g_app, commit);
}

static char *csv_field(char **cursor)
{
    if (!cursor || !*cursor)
        return NULL;
    char *field = *cursor;
    char *comma = strchr(field, ',');
    if (comma) {
        *comma = 0;
        *cursor = comma + 1;
    } else {
        *cursor = NULL;
    }
    return field;
}

static void parse_event(char *line)
{
    char *sep = strstr(line, ">>");
    if (!sep)
        return;
    *sep = 0;
    char *data = sep + 2;
    char *nl = strpbrk(data, "\r\n");
    if (nl) *nl = 0;
    if (!strcmp(line, "snapshot")) {
        char *cursor = data;
        char *a = csv_field(&cursor);
        char *b = csv_field(&cursor);
        char *c = csv_field(&cursor);
        char *d = csv_field(&cursor);
        char *e = cursor; /* titulo pode conter virgulas */
        if (a) g_active = atoi(a);
        if (b) { strncpy(g_layout, b, sizeof g_layout - 1); g_layout[sizeof g_layout - 1] = 0; }
        unsigned occupied = c ? (unsigned)strtoul(c, NULL, 16) : 0;
        unsigned urgent = d ? (unsigned)strtoul(d, NULL, 16) : 0;
        for (int i = 0; i < NTUWM_WS; i++) {
            g_occupied[i] = (occupied & (1u << i)) != 0;
            g_urgent[i] = (urgent & (1u << i)) != 0;
        }
        g_title[0] = 0;
        if (e) {
            strncpy(g_title, e, sizeof g_title - 1);
            g_title[sizeof g_title - 1] = 0;
        }
    } else if (!strcmp(line, "workspace-state")) {
        char *cursor = data;
        char *a = csv_field(&cursor);
        char *b = csv_field(&cursor);
        char *c = csv_field(&cursor);
        char *d = cursor; /* nome pode conter virgulas */
        int i = a ? atoi(a) - 1 : -1;
        if (i >= 0 && i < NTUWM_WS) {
            g_occupied[i] = b ? atoi(b) : 0;
            g_urgent[i] = c ? atoi(c) : 0;
            if (d) { strncpy(g_names[i], d, sizeof g_names[i] - 1); g_names[i][sizeof g_names[i] - 1] = 0; }
        }
    } else if (!strcmp(line, "workspace")) {
        g_active = atoi(data);
        ctl("snapshot");
    } else if (!strcmp(line, "client-added") ||
               !strcmp(line, "client-removed") ||
               !strcmp(line, "frame")) {
        /* frame cobre movetoworkspace, que muda duas ocupacoes de uma vez. */
        ctl("snapshot");
    } else if (!strcmp(line, "layout")) {
        strncpy(g_layout, data, sizeof g_layout - 1);
        g_layout[sizeof g_layout - 1] = 0;
    } else if (!strcmp(line, "focus") || !strcmp(line, "title")) {
        strncpy(g_title, data, sizeof g_title - 1);
        g_title[sizeof g_title - 1] = 0;
    }
}

static void pointer_action(int x, int button, int state, int axis)
{
    if (axis) {
        int ws = next_visible_workspace(g_active, axis > 0 ? -1 : 1);
        if (ws == g_active)
            return;
        char cmd[48];
        snprintf(cmd, sizeof cmd, "workspace %d", ws);
        ctl(cmd);
        return;
    }
    if (button != 0 || state != 1)
        return;
    for (int i = 0; i < NTUWM_WS; i++)
        if (x >= g_ws_x0[i] && x < g_ws_x1[i]) {
            char cmd[48];
            snprintf(cmd, sizeof cmd, "workspace %d", i + 1);
            ctl(cmd);
            return;
        }
    if (x >= g_layout_x0 && x < g_layout_x1)
        ctl("layout toggle");
}

static int handle_app_message(char *buf)
{
    if (!strncmp(buf, APP_EVT_CONFIGURE " ", strlen(APP_EVT_CONFIGURE) + 1)) {
        unsigned serial;
        char name[80];
        int w, h;
        if (sscanf(buf, APP_EVT_CONFIGURE " %u %79s %d %d",
                   &serial, name, &w, &h) == 4 &&
            surface_configure(serial, name, w, h)) {
            render();
            return 1;
        }
        return 0;
    }
    if (!strncmp(buf, APP_EVT_POINTER " ", strlen(APP_EVT_POINTER) + 1)) {
        int x, y, buttons, button, state, axis;
        if (sscanf(buf, APP_EVT_POINTER " %d %d %d %d %d %d",
                   &x, &y, &buttons, &button, &state, &axis) == 6) {
            (void)y; (void)buttons;
            pointer_action(x, button, state, axis);
        }
    } else if (!strcmp(buf, APP_EVT_CLOSE)) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        g_active = 1;
        ZeroMemory(g_occupied, sizeof g_occupied);
        ZeroMemory(g_urgent, sizeof g_urgent);
        if (!workspace_visible(0) || workspace_visible(1))
            return 1;
        g_occupied[2] = 1;
        if (!workspace_visible(2) ||
            next_visible_workspace(1, 1) != 3 ||
            next_visible_workspace(3, -1) != 1)
            return 2;
        g_occupied[2] = 0;
        g_active = 2;
        if (workspace_visible(0) || !workspace_visible(1))
            return 3;
        char snapshot[] = "snapshot>>2,master,5,4,title,with,commas\n";
        parse_event(snapshot);
        if (g_active != 2 || strcmp(g_layout, "master") ||
            !g_occupied[0] || !g_occupied[2] || g_occupied[1] ||
            !g_urgent[2] || strcmp(g_title, "title,with,commas"))
            return 4;
        return 0;
    }
    for (int i = 0; i < NTUWM_WS; i++)
        snprintf(g_names[i], sizeof g_names[i], "%d", i + 1);
    g_app = connect_pipe(NTU_PIPE_DISPD_APP, GENERIC_READ | GENERIC_WRITE);
    if (g_app == INVALID_HANDLE_VALUE)
        return 1;
    char hello[160];
    snprintf(hello, sizeof hello, "%s %d %s 0 34 0x%x 34 %d ntbar",
             APP_CMD_HELLO, NTUAPP_PROTO_VER, NTUAPP_ROLE_LAYER,
             NTUAPP_ANCHOR_TOP | NTUAPP_ANCHOR_LEFT | NTUAPP_ANCHOR_RIGHT,
             NTUAPP_INTERACT_NONE);
    if (!write_msg(g_app, hello))
        return 1;

    char buf[1024];
    int n = read_msg(g_app, buf, sizeof buf, 1); /* APP-WELCOME */
    if (n <= 0 || strncmp(buf, APP_EVT_WELCOME, strlen(APP_EVT_WELCOME)))
        return 1;
    n = read_msg(g_app, buf, sizeof buf, 1);     /* APP-CONFIGURE */
    if (n <= 0 || !handle_app_message(buf))
        return 1;

    ULONGLONG last_clock = 0, last_event_retry = 0;
    while (g_app != INVALID_HANDLE_VALUE) {
        n = read_msg(g_app, buf, sizeof buf, 0);
        if (n < 0)
            break;
        if (n > 0 && !handle_app_message(buf))
            break;

        if (g_events == INVALID_HANDLE_VALUE &&
            GetTickCount64() - last_event_retry > 1000) {
            last_event_retry = GetTickCount64();
            g_events = connect_pipe(NTU_PIPE_NTWM_EVENTS, GENERIC_READ);
            if (g_events != INVALID_HANDLE_VALUE) {
                Sleep(20); /* accept thread publica a conexao antes do snapshot */
                ctl("snapshot");
            }
        }
        if (g_events != INVALID_HANDLE_VALUE) {
            n = read_msg(g_events, buf, sizeof buf, 0);
            if (n < 0) {
                CloseHandle(g_events);
                g_events = INVALID_HANDLE_VALUE;
            } else if (n > 0) {
                parse_event(buf);
                render();
            }
        }
        if (GetTickCount64() - last_clock >= 1000) {
            last_clock = GetTickCount64();
            render();
        }
        Sleep(16);
    }
    if (g_events != INVALID_HANDLE_VALUE) CloseHandle(g_events);
    if (g_app != INVALID_HANDLE_VALUE) CloseHandle(g_app);
    surface_drop();
    return 0;
}
