/*
 * ntdbgcon.c — console de debug via REDE (reverse shell TCP). SO EM BUILD DE DEV.
 *
 * A serial ISA/PCI nao sobe no WinPE UEFI (ISA: sem NTDETECT; PCI: INF nao assinado
 * barrado por 0xE000022F, e a injecao offline precisa de DISM/Windows). A rede, ao
 * contrario, usa o driver e1000e INBOX (assinado pela MS) -> sobe sozinha no boot
 * (validado: ipconfig mostra 10.0.2.15).
 *
 * Fluxo (reverse, guest->host, driblando o SLIRP que bloqueia host->guest):
 *   host:  nc -lv 127.0.0.1 2323
 *   guest: conecta em 10.0.2.2:2323 (o gateway do SLIRP encaminha p/ o host); retry
 *   ao conectar: spawna 'busybox sh -i' e faz RELAY socket <-> pipes <-> shell
 *   ao cair a conexao / sair o shell: fecha tudo e tenta de novo
 *
 * O socket fica 100% aqui (mingw/Winsock); o busybox so recebe PIPES anonimos como
 * stdio (NT_FD_PIPE) — evita depender de Winsock-como-fd na musl-nt. O relay converte
 * CR->LF (trata CR, LF e CRLF) pra o shell executar tanto com \r quanto \n.
 *
 * DEV only: gate no unit (marker enabled/ntdbgcon so com NTUNIX_DEBUG=1). Listener no
 * host preso em 127.0.0.1; sem exposicao em bridge/LAN.
 */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#define DBG_HOST "10.0.2.2"      /* gateway do SLIRP = host */
#define DBG_PORT 2323

static char g_logpath[MAX_PATH];
static char g_dir[MAX_PATH];

/* estado compartilhado com as threads de relay (uma conexao por vez -> global ok) */
static SOCKET g_sock;
static HANDLE g_pin_w;           /* escreve no stdin do shell */
static HANDLE g_pout_r;          /* le do stdout do shell */

static void logln(const char *msg, unsigned long code)
{
    HANDLE h;
    char line[256];
    int n = 0;
    const char *p;
    DWORD w;
    for (p = msg; *p && n < 200; p++) line[n++] = *p;
    if (code != 0xFFFFFFFFu) {
        int i;
        line[n++] = ' '; line[n++] = '('; line[n++] = 'e'; line[n++] = '=';
        for (i = 28; i >= 0; i -= 4) line[n++] = "0123456789abcdef"[(code >> i) & 0xf];
        line[n++] = ')';
    }
    line[n++] = '\r'; line[n++] = '\n';
    h = CreateFileA(g_logpath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_ALWAYS, 0, NULL);
    if (h != INVALID_HANDLE_VALUE) { WriteFile(h, line, (DWORD)n, &w, NULL); CloseHandle(h); }
}

/* socket -> stdin do shell, com CR->LF (CR vira LF; LF logo apos CR e' descartado) */
static DWORD WINAPI sock_to_child(LPVOID arg)
{
    char in[1024], out[1024];
    int prev_cr = 0;
    (void)arg;
    for (;;) {
        int n = recv(g_sock, in, sizeof in, 0);
        int i, m = 0;
        DWORD w;
        if (n <= 0) break;
        for (i = 0; i < n; i++) {
            char c = in[i];
            if (c == '\r') { out[m++] = '\n'; prev_cr = 1; }
            else if (c == '\n' && prev_cr) { prev_cr = 0; }
            else { out[m++] = c; prev_cr = 0; }
        }
        if (m && !WriteFile(g_pin_w, out, (DWORD)m, &w, NULL)) break;
    }
    return 0;
}

/* stdout do shell -> socket (verbatim) */
static DWORD WINAPI child_to_sock(LPVOID arg)
{
    char buf[1024];
    DWORD n;
    (void)arg;
    for (;;) {
        if (!ReadFile(g_pout_r, buf, sizeof buf, &n, NULL) || n == 0) break;
        if (send(g_sock, buf, (int)n, 0) <= 0) break;
    }
    return 0;
}

int main(void)
{
    WSADATA wsa;
    char *slash, *p, busybox[MAX_PATH];
    DWORD nmod;

    nmod = GetModuleFileNameA(NULL, g_dir, sizeof g_dir);
    if (nmod == 0 || nmod >= sizeof g_dir) return 1;
    slash = g_dir;
    for (p = g_dir; *p; p++) if (*p == '\\' || *p == '/') slash = p;
    *slash = 0;
    lstrcpyA(g_logpath, g_dir); lstrcatA(g_logpath, "\\ntdbgcon.log");
    lstrcpyA(busybox, g_dir);   lstrcatA(busybox, "\\busybox.exe");

    logln("ntdbgcon: reverse-shell iniciando", 0xFFFFFFFFu);
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        logln("ntdbgcon: WSAStartup falhou", (unsigned long)WSAGetLastError());
        return 1;
    }

    for (;;) {
        SOCKET s;
        struct sockaddr_in sa;
        HANDLE pin_r = NULL, pin_w = NULL, pout_r = NULL, pout_w = NULL;
        SECURITY_ATTRIBUTES sec;
        STARTUPINFOA si;
        PROCESS_INFORMATION pi;
        char cmd[MAX_PATH + 16];
        HANDLE t1, t2;
        DWORD ec = 0;

        s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) { Sleep(2000); continue; }
        ZeroMemory(&sa, sizeof sa);
        sa.sin_family = AF_INET;
        sa.sin_port = htons(DBG_PORT);
        sa.sin_addr.s_addr = inet_addr(DBG_HOST);
        if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
            closesocket(s);
            Sleep(2000);   /* host nao esta escutando ainda -> retry */
            continue;
        }
        logln("ntdbgcon: conectado ao host 10.0.2.2:2323", 0xFFFFFFFFu);

        /* pipes anonimos herdaveis; pontas do pai marcadas NAO-herdaveis */
        sec.nLength = sizeof sec; sec.lpSecurityDescriptor = NULL; sec.bInheritHandle = TRUE;
        if (!CreatePipe(&pin_r, &pin_w, &sec, 0) ||
            !CreatePipe(&pout_r, &pout_w, &sec, 0)) {
            logln("ntdbgcon: CreatePipe falhou", GetLastError());
            closesocket(s); Sleep(1000); continue;
        }
        SetHandleInformation(pin_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(pout_r, HANDLE_FLAG_INHERIT, 0);

        ZeroMemory(&si, sizeof si); si.cb = sizeof si;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = pin_r;      /* shell le do pin */
        si.hStdOutput = pout_w;     /* shell escreve no pout */
        si.hStdError  = pout_w;
        cmd[0] = '"'; lstrcpyA(cmd + 1, busybox); lstrcatA(cmd, "\" sh -i");

        ZeroMemory(&pi, sizeof pi);
        if (!CreateProcessA(busybox, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
            logln("ntdbgcon: CreateProcess(busybox) falhou", GetLastError());
            CloseHandle(pin_r); CloseHandle(pin_w);
            CloseHandle(pout_r); CloseHandle(pout_w);
            closesocket(s);
            Sleep(1000);
            continue;
        }
        logln("ntdbgcon: shell spawnado — relay ativo", 0xFFFFFFFFu);

        /* pontas do filho ja herdadas: o pai as fecha (senao o EOF nunca chega) */
        CloseHandle(pin_r);  pin_r  = NULL;
        CloseHandle(pout_w); pout_w = NULL;

        g_sock = s; g_pin_w = pin_w; g_pout_r = pout_r;
        t1 = CreateThread(NULL, 0, sock_to_child, NULL, 0, NULL);
        t2 = CreateThread(NULL, 0, child_to_sock, NULL, 0, NULL);

        /* espera o shell sair (o usuario mandou 'exit' ou a conexao caiu) */
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &ec);
        logln("ntdbgcon: shell saiu (exit)", ec);

        /* derruba as threads: fecha socket + pipes -> recv/ReadFile retornam */
        shutdown(s, SD_BOTH);
        closesocket(s);
        CloseHandle(pin_w);
        CloseHandle(pout_r);
        if (t1) { WaitForSingleObject(t1, 2000); CloseHandle(t1); }
        if (t2) { WaitForSingleObject(t2, 2000); CloseHandle(t2); }
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        Sleep(500);   /* nova conexao */
    }
}
