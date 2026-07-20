/*
 * ipc.c - IPC publico do ntwm.
 *
 * Pipe 1: request/response para ntwmctl.
 * Pipe 2: stream somente de saida para ntbar e consumidores futuros.
 *
 * O buffer do named pipe (64 KiB) e a fila limitada. Uma escrita que nao
 * progride em 5 ms desconecta o consumidor: politica nunca bloqueia no pager.
 */
#include "ntwm.h"

#define CTL_QCAP 32
#define EVENT_CLIENTS 16

typedef struct CtlRequest {
    char   command[512];
    char   reply[1024];
    HANDLE done;
} CtlRequest;

typedef struct EventClient {
    HANDLE pipe;
    HANDLE event;
    struct EventClient *next;
} EventClient;

static CtlRequest *g_ctlq[CTL_QCAP];
static int g_ctlqh, g_ctlqt;
static CRITICAL_SECTION g_ctllock;
static CRITICAL_SECTION g_eventlock;
static EventClient *g_events;
static HANDLE g_ctl_thread, g_event_thread;
static volatile LONG g_ipc_ready;
static volatile LONG g_ipc_running;

static int same_session(HANDLE pipe)
{
    ULONG pid = 0;
    DWORD client_session = (DWORD)-1, our_session = (DWORD)-2;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid)
        return 0;
    if (!ProcessIdToSessionId((DWORD)pid, &client_session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &our_session))
        return 0;
    return client_session == our_session;
}

static int ctl_push(CtlRequest *r)
{
    int ok = 0;
    EnterCriticalSection(&g_ctllock);
    int nt = (g_ctlqt + 1) % CTL_QCAP;
    if (nt != g_ctlqh) {
        g_ctlq[g_ctlqt] = r;
        g_ctlqt = nt;
        ok = 1;
    }
    LeaveCriticalSection(&g_ctllock);
    return ok;
}

static CtlRequest *ctl_pop(void)
{
    CtlRequest *r = NULL;
    EnterCriticalSection(&g_ctllock);
    if (g_ctlqh != g_ctlqt) {
        r = g_ctlq[g_ctlqh];
        g_ctlqh = (g_ctlqh + 1) % CTL_QCAP;
    }
    LeaveCriticalSection(&g_ctllock);
    return r;
}

static DWORD WINAPI ctl_main(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_ipc_running, 0, 0)) {
        HANDLE pipe = CreateNamedPipeA(
            NTU_PIPE_NTWM_CTL, PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
            4, 4096, 4096, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }
        BOOL connected = ConnectNamedPipe(pipe, NULL)
            ? TRUE : GetLastError() == ERROR_PIPE_CONNECTED;
        if (!connected || !same_session(pipe)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        CtlRequest *r = (CtlRequest *)calloc(1, sizeof *r);
        if (!r) {
            DWORD written;
            WriteFile(pipe, "error sem memoria", 17, &written, NULL);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(pipe, r->command, sizeof r->command - 1, &n, NULL) || !n) {
            free(r);
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        r->command[n] = 0;
        ntu_trim(r->command);
        r->done = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!r->done || !ctl_push(r)) {
            DWORD written;
            const char *msg = "error fila cheia";
            WriteFile(pipe, msg, (DWORD)strlen(msg), &written, NULL);
            if (r->done) CloseHandle(r->done);
            free(r);
        } else {
            WaitForSingleObject(r->done, INFINITE);
            DWORD written;
            WriteFile(pipe, r->reply, (DWORD)strlen(r->reply), &written, NULL);
            CloseHandle(r->done);
            free(r);
        }
        FlushFileBuffers(pipe);
        DisconnectNamedPipe(pipe);
        CloseHandle(pipe);
    }
    return 0;
}

static void event_client_add(HANDLE pipe)
{
    EventClient *c = (EventClient *)calloc(1, sizeof *c);
    if (!c) {
        CloseHandle(pipe);
        return;
    }
    c->event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!c->event) {
        free(c);
        CloseHandle(pipe);
        return;
    }
    c->pipe = pipe;
    EnterCriticalSection(&g_eventlock);
    int count = 0;
    for (EventClient *it = g_events; it; it = it->next)
        count++;
    if (count >= EVENT_CLIENTS) {
        LeaveCriticalSection(&g_eventlock);
        CloseHandle(c->event);
        CloseHandle(c->pipe);
        free(c);
        return;
    }
    c->next = g_events;
    g_events = c;
    LeaveCriticalSection(&g_eventlock);
}

static DWORD WINAPI event_accept_main(LPVOID arg)
{
    (void)arg;
    while (InterlockedCompareExchange(&g_ipc_running, 0, 0)) {
        HANDLE pipe = CreateNamedPipeA(
            NTU_PIPE_NTWM_EVENTS,
            PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS,
            EVENT_CLIENTS, 65536, 1024, 0, NULL);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(100);
            continue;
        }
        HANDLE ev = CreateEventA(NULL, TRUE, FALSE, NULL);
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = ev;
        BOOL connected = ConnectNamedPipe(pipe, &ov);
        if (!connected && GetLastError() == ERROR_IO_PENDING) {
            WaitForSingleObject(ev, INFINITE);
            DWORD dummy = 0;
            connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE);
        } else if (!connected && GetLastError() == ERROR_PIPE_CONNECTED) {
            connected = TRUE;
        }
        CloseHandle(ev);
        if (!connected || !same_session(pipe)) {
            DisconnectNamedPipe(pipe);
            CloseHandle(pipe);
            continue;
        }
        event_client_add(pipe);
    }
    return 0;
}

static void event_line(const char *event, const char *data, char *out, size_t cap)
{
    snprintf(out, cap, "%s>>%s\n", event, data ? data : "");
    char *payload = strstr(out, ">>");
    if (payload)
        for (payload += 2; *payload; payload++)
            if (*payload == '\r' || (*payload == '\n' && payload[1]))
                *payload = ' ';
}

void wm_ipc_event(const char *event, const char *data)
{
    if (!InterlockedCompareExchange(&g_ipc_ready, 0, 0))
        return;
    char line[1400];
    event_line(event, data, line, sizeof line);
    EnterCriticalSection(&g_eventlock);
    EventClient **pp = &g_events;
    while (*pp) {
        EventClient *c = *pp;
        OVERLAPPED ov;
        ZeroMemory(&ov, sizeof ov);
        ov.hEvent = c->event;
        ResetEvent(c->event);
        DWORD written = 0;
        BOOL ok = WriteFile(c->pipe, line, (DWORD)strlen(line), &written, &ov);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(c->event, 5) == WAIT_OBJECT_0)
                ok = GetOverlappedResult(c->pipe, &ov, &written, FALSE);
            else {
                CancelIoEx(c->pipe, &ov);
                GetOverlappedResult(c->pipe, &ov, &written, TRUE);
                ok = FALSE;
            }
        }
        if (!ok || written != strlen(line)) {
            *pp = c->next;
            DisconnectNamedPipe(c->pipe);
            CloseHandle(c->pipe);
            CloseHandle(c->event);
            free(c);
            continue;
        }
        pp = &c->next;
    }
    LeaveCriticalSection(&g_eventlock);
}

void wm_ipc_snapshot(void)
{
    char data[768];
    Workspace *ws = wm_workspace(g_wm.cur_ws);
    unsigned occupied_mask = 0;
    unsigned urgent_mask = 0;
    for (int i = 0; i < NTUWM_WS; i++) {
        if (g_wm.workspaces[i].clients)
            occupied_mask |= 1u << i;
        for (Client *c = g_wm.workspaces[i].clients; c; c = c->ws_next)
            if (c->urgent) {
                urgent_mask |= 1u << i;
                break;
            }
    }
    /* Estado necessario para renderizar a barra vai em UMA mensagem. Assim um
     * consumidor nunca combina o workspace novo com ocupacao antiga. */
    snprintf(data, sizeof data, "%d,%s,%x,%x,%s", g_wm.cur_ws + 1,
             ws && ws->layout == LAYOUT_MASTER ? "master" : "dwindle",
             occupied_mask, urgent_mask,
             ws && ws->focused ? ws->focused->title : "");
    wm_ipc_event("snapshot", data);
    for (int i = 0; i < NTUWM_WS; i++) {
        int occupied = g_wm.workspaces[i].clients != NULL;
        int urgent = 0;
        for (Client *c = g_wm.workspaces[i].clients; c; c = c->ws_next)
            urgent |= c->urgent;
        snprintf(data, sizeof data, "%d,%d,%d,%s", i + 1, occupied, urgent,
                 g_wm.workspaces[i].name);
        wm_ipc_event("workspace-state", data);
    }
}

void wm_ipc_drain(void)
{
    CtlRequest *r;
    while ((r = ctl_pop()) != NULL) {
        char command[sizeof r->command];
        strncpy(command, r->command, sizeof command - 1);
        command[sizeof command - 1] = 0;
        char *action = command;
        while (*action == ' ' || *action == '\t') action++;
        char *arg = strchr(action, ' ');
        if (arg) {
            *arg++ = 0;
            while (*arg == ' ' || *arg == '\t') arg++;
        } else {
            arg = "";
        }
        if (!_stricmp(action, "dispatch")) {
            action = arg;
            arg = strchr(action, ' ');
            if (arg) {
                *arg++ = 0;
                while (*arg == ' ' || *arg == '\t') arg++;
            } else {
                arg = "";
            }
        }
        wm_dispatch(action, arg, r->reply, sizeof r->reply);
        SetEvent(r->done);
    }
}

void wm_ipc_start(void)
{
    InitializeCriticalSection(&g_ctllock);
    InitializeCriticalSection(&g_eventlock);
    InterlockedExchange(&g_ipc_running, 1);
    InterlockedExchange(&g_ipc_ready, 1);
    g_ctl_thread = CreateThread(NULL, 0, ctl_main, NULL, 0, NULL);
    g_event_thread = CreateThread(NULL, 0, event_accept_main, NULL, 0, NULL);
}

void wm_ipc_stop(void)
{
    InterlockedExchange(&g_ipc_running, 0);
    InterlockedExchange(&g_ipc_ready, 0);
    EnterCriticalSection(&g_eventlock);
    while (g_events) {
        EventClient *c = g_events;
        g_events = c->next;
        CancelIoEx(c->pipe, NULL);
        DisconnectNamedPipe(c->pipe);
        CloseHandle(c->pipe);
        CloseHandle(c->event);
        free(c);
    }
    LeaveCriticalSection(&g_eventlock);
    /* Threads de accept sao encerrados pelo teardown do processo. Os handles
     * nao ficam herdaveis e nenhum deles toca o modelo depois de ipc_ready=0. */
    if (g_ctl_thread) CloseHandle(g_ctl_thread);
    if (g_event_thread) CloseHandle(g_event_thread);
}
