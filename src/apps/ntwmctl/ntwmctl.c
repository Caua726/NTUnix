/* ntwmctl - cliente request/response dos dispatchers do ntwm. */
#include "../../common/ntu.h"

static HANDLE connect_ctl(void)
{
    for (int i = 0; i < 40; i++) {
        HANDLE h = CreateFileA(NTU_PIPE_NTWM_CTL,
                               GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(h, &mode, NULL, NULL);
            return h;
        }
        if (GetLastError() == ERROR_PIPE_BUSY)
            WaitNamedPipeA(NTU_PIPE_NTWM_CTL, 500);
        else
            Sleep(50);
    }
    return INVALID_HANDLE_VALUE;
}

int main(int argc, char **argv)
{
    char command[512] = "status";
    if (argc > 1) {
        command[0] = 0;
        for (int i = 1; i < argc; i++) {
            size_t have = strlen(command), need = strlen(argv[i]);
            if (have + need + 2 >= sizeof command) {
                fputs("ntwmctl: comando muito longo\n", stderr);
                return 2;
            }
            if (have) strcat(command, " ");
            strcat(command, argv[i]);
        }
    }
    HANDLE pipe = connect_ctl();
    if (pipe == INVALID_HANDLE_VALUE) {
        fputs("ntwmctl: ntwm indisponivel\n", stderr);
        return 1;
    }
    DWORD n;
    if (!WriteFile(pipe, command, (DWORD)strlen(command), &n, NULL)) {
        CloseHandle(pipe);
        return 1;
    }
    char reply[1024];
    if (!ReadFile(pipe, reply, sizeof reply - 1, &n, NULL) || !n) {
        CloseHandle(pipe);
        return 1;
    }
    reply[n] = 0;
    puts(reply);
    CloseHandle(pipe);
    return strncmp(reply, "ok", 2) ? 1 : 0;
}
