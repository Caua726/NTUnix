#include "nt/ntpriv.h"

/* fork() via RtlCloneUserProcess — o fork nativo do NT (wrapper de
 * NtCreateUserProcess), o mesmo caminho que os subsistemas Unix-on-NT usam.
 * Undocumented mas estável; retorna STATUS_PROCESS_CLONED (0x129) no filho.
 * Ausente no Wine => fork indisponível lá (ENOSYS); presente no Windows real
 * (inclusive no WinPE da NTUnix-live), que é onde isso se valida.
 *
 * IMPORTANTE: o clone suporta rodar código Unix in-process (read/write/pipe/
 * getdents/malloc/console funcionam), mas NÃO suporta CreateProcessW — ele
 * crasha dentro do processo clonado (o CSR/loader fica parcial). Por isso o
 * BusyBox roda TODO applet in-process (patch no ash), em vez de fork+exec. */
#define NT_RTL_CLONE_CREATE_SUSPENDED 0x00000001UL
#define NT_RTL_CLONE_INHERIT_HANDLES  0x00000002UL
#define NT_STATUS_PROCESS_CLONED      ((LONG)0x00000129)

typedef struct {
    ULONG Length;
    HANDLE ProcessHandle;
    HANDLE ThreadHandle;
    CLIENT_ID ClientId;
    unsigned char ImageInformation[128]; /* SECTION_IMAGE_INFORMATION + folga */
} nt_clone_info;

typedef LONG (NTAPI *nt_clone_fn)(ULONG, PVOID, PVOID, HANDLE, PVOID);

/* ---- tabela de processos filhos: o fork registra (pid,handle) aqui e o
 * wait4 usa — inclusive waitpid(-1) (esperar QUALQUER filho), que o ash
 * precisa para coordenar os estágios de um pipeline. ---- */
#define NT_MAX_CHILDREN 256
static struct { DWORD pid; HANDLE handle; } g_children[NT_MAX_CHILDREN];
static SRWLOCK g_children_lock = SRWLOCK_INIT;

static void child_add(DWORD pid, HANDLE h)
{
    int i;
    AcquireSRWLockExclusive(&g_children_lock);
    for (i = 0; i < NT_MAX_CHILDREN; ++i)
        if (!g_children[i].handle) {
            g_children[i].pid = pid;
            g_children[i].handle = h;
            break;
        }
    ReleaseSRWLockExclusive(&g_children_lock);
    if (i == NT_MAX_CHILDREN) CloseHandle(h); /* tabela cheia: não guarda */
}

nt_sc_t nt_sys_exit(nt_sc_t code)
{
    ExitProcess((UINT)code);
    for (;;) {}
}

nt_sc_t nt_sys_getpid(void)
{
    return (nt_sc_t)GetCurrentProcessId();
}

nt_sc_t nt_sys_getppid(void)
{
    PROCESS_BASIC_INFORMATION info;
    ULONG returned;
    NTSTATUS status = NtQueryInformationProcess(GetCurrentProcess(),
                                                 ProcessBasicInformation,
                                                 &info, sizeof info, &returned);
    if (status < 0) return nt_error_from_status(status);
    return (nt_sc_t)(uintptr_t)info.InheritedFromUniqueProcessId;
}

nt_sc_t nt_sys_set_tid_address(nt_sc_t ptr)
{
    (void)ptr;
    return (nt_sc_t)GetCurrentThreadId();
}

static int append_wchar(WCHAR *dst, size_t cap, size_t *used, WCHAR value)
{
    if (*used + 1 >= cap) return 0;
    dst[(*used)++] = value;
    dst[*used] = 0;
    return 1;
}

/* Windows command-line quoting compatible with CommandLineToArgvW. */
static int append_argument(WCHAR *dst, size_t cap, size_t *used,
                           const WCHAR *arg)
{
    size_t slashes = 0;
    const WCHAR *p;
    if (*used && !append_wchar(dst, cap, used, L' ')) return 0;
    if (!append_wchar(dst, cap, used, L'"')) return 0;
    for (p = arg;; ++p) {
        if (*p == L'\\') {
            ++slashes;
            continue;
        }
        if (*p == L'"' || !*p) {
            size_t i, count = slashes * 2 + (*p == L'"' ? 1 : 0);
            for (i = 0; i < count; ++i)
                if (!append_wchar(dst, cap, used, L'\\')) return 0;
            slashes = 0;
            if (!*p) break;
            if (!append_wchar(dst, cap, used, L'"')) return 0;
        } else {
            size_t i;
            for (i = 0; i < slashes; ++i)
                if (!append_wchar(dst, cap, used, L'\\')) return 0;
            slashes = 0;
            if (!append_wchar(dst, cap, used, *p)) return 0;
        }
    }
    return append_wchar(dst, cap, used, L'"');
}

static nt_sc_t build_command_line(char *const *argv, WCHAR *cmd, size_t cap)
{
    size_t used = 0, i;
    if (!argv || !argv[0]) return -NT_EFAULT;
    cmd[0] = 0;
    for (i = 0; argv[i]; ++i) {
        WCHAR arg[NT_PATH_CAP];
        nt_sc_t r = nt_utf8_to_wide(argv[i], arg, NT_ARRAY_LEN(arg));
        if (r < 0) return r;
        if (!append_argument(cmd, cap, &used, arg)) return -NT_E2BIG;
    }
    return 0;
}

static nt_sc_t build_environment(char *const *envp, WCHAR *block, size_t cap)
{
    size_t used = 0, i;
    if (!envp) return 0;
    for (i = 0; envp[i]; ++i) {
        nt_sc_t n;
        if (used + 2 >= cap) return -NT_E2BIG;
        n = nt_utf8_to_wide(envp[i], block + used, cap - used);
        if (n < 0) return n;
        used += (size_t)n + 1;
    }
    block[used++] = 0;
    if (used < cap) block[used] = 0;
    return 0;
}

/* execve = spawn-e-espera (CreateProcessW + wait + ExitProcess). NOTA: quando
 * chamado a partir de um filho de fork() (o clone), o CreateProcessW crasha —
 * limitação do RtlCloneUserProcess. Serve para exec a partir do processo
 * principal; o BusyBox evita fork+exec rodando applets in-process. */
nt_sc_t nt_sys_execve(nt_sc_t path_arg, nt_sc_t argv_arg, nt_sc_t envp_arg)
{
    const char *path_utf8 = (const char *)(uintptr_t)path_arg;
    char *const *argv = (char *const *)(uintptr_t)argv_arg;
    char *const *envp = (char *const *)(uintptr_t)envp_arg;
    WCHAR path[NT_PATH_CAP];
    WCHAR *cmd, *environment;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD exit_code;
    nt_sc_t r;
    if (!path_utf8 || !argv) return -NT_EFAULT;
    r = nt_path_at(NT_AT_FDCWD, path_utf8, path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    cmd = VirtualAlloc(0, 65536 * sizeof(WCHAR), MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    environment = VirtualAlloc(0, 524288 * sizeof(WCHAR),
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cmd || !environment) {
        if (cmd) VirtualFree(cmd, 0, MEM_RELEASE);
        if (environment) VirtualFree(environment, 0, MEM_RELEASE);
        return -NT_ENOMEM;
    }
    r = build_command_line(argv, cmd, 65536);
    if (!r) r = build_environment(envp, environment, 524288);
    if (r < 0) {
        VirtualFree(cmd, 0, MEM_RELEASE);
        VirtualFree(environment, 0, MEM_RELEASE);
        return r;
    }
    nt_memset(&startup, 0, sizeof startup);
    nt_memset(&process, 0, sizeof process);
    startup.cb = sizeof startup;
    /* Passa fd 0/1/2 explicitamente como std handles do filho quando há
     * redirecionamento real (algum de 0/1/2 é pipe/arquivo/socket). Sem isto o
     * processo criado só herdaria o console, perdendo redirects e pipelines.
     * Se os três forem console, deixa a herança automática do console. */
    {
        struct nt_fd *s0 = nt_fd_get(0), *s1 = nt_fd_get(1), *s2 = nt_fd_get(2);
        int redirected = (s0 && s0->kind != NT_FD_CONSOLE) ||
                         (s1 && s1->kind != NT_FD_CONSOLE) ||
                         (s2 && s2->kind != NT_FD_CONSOLE);
        if (redirected && s0 && s1 && s2) {
            SetHandleInformation(s0->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(s1->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(s2->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdInput = s0->handle;
            startup.hStdOutput = s1->handle;
            startup.hStdError = s2->handle;
        }
    }
    if (!CreateProcessW(path, cmd, 0, 0, TRUE, CREATE_UNICODE_ENVIRONMENT,
                        envp ? environment : 0, 0, &startup, &process)) {
        r = nt_last_error();
        VirtualFree(cmd, 0, MEM_RELEASE);
        VirtualFree(environment, 0, MEM_RELEASE);
        return r;
    }
    CloseHandle(process.hThread);
    WaitForSingleObject(process.hProcess, INFINITE);
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hProcess);
    VirtualFree(cmd, 0, MEM_RELEASE);
    VirtualFree(environment, 0, MEM_RELEASE);
    ExitProcess(exit_code);
    for (;;) {}
}

/* posix_spawn: CreateProcessW no PROCESSO ATUAL (nunca um clone), então
 * funciona onde fork+exec falha (CreateProcessW crasha dentro do clone do
 * RtlCloneUserProcess). Registra o filho em g_children pro wait4 e devolve o
 * pid; ao contrário do execve, NÃO espera nem faz ExitProcess. O caller (o
 * override posix_spawn) já aplicou as file_actions nos fds 0/1/2 correntes,
 * que viram os std handles do filho. */
nt_sc_t nt_sys_spawn(nt_sc_t path_arg, nt_sc_t argv_arg, nt_sc_t envp_arg)
{
    const char *path_utf8 = (const char *)(uintptr_t)path_arg;
    char *const *argv = (char *const *)(uintptr_t)argv_arg;
    char *const *envp = (char *const *)(uintptr_t)envp_arg;
    WCHAR path[NT_PATH_CAP];
    WCHAR *cmd, *environment;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    nt_sc_t r;
    if (!path_utf8 || !argv) return -NT_EFAULT;
    r = nt_path_at(NT_AT_FDCWD, path_utf8, path, NT_ARRAY_LEN(path));
    if (r < 0) return r;
    cmd = VirtualAlloc(0, 65536 * sizeof(WCHAR), MEM_RESERVE | MEM_COMMIT,
                       PAGE_READWRITE);
    environment = VirtualAlloc(0, 524288 * sizeof(WCHAR),
                               MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cmd || !environment) {
        if (cmd) VirtualFree(cmd, 0, MEM_RELEASE);
        if (environment) VirtualFree(environment, 0, MEM_RELEASE);
        return -NT_ENOMEM;
    }
    r = build_command_line(argv, cmd, 65536);
    if (!r) r = build_environment(envp, environment, 524288);
    if (r < 0) {
        VirtualFree(cmd, 0, MEM_RELEASE);
        VirtualFree(environment, 0, MEM_RELEASE);
        return r;
    }
    nt_memset(&startup, 0, sizeof startup);
    nt_memset(&process, 0, sizeof process);
    startup.cb = sizeof startup;
    {
        struct nt_fd *s0 = nt_fd_get(0), *s1 = nt_fd_get(1), *s2 = nt_fd_get(2);
        if (s0 && s1 && s2) {
            SetHandleInformation(s0->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(s1->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            SetHandleInformation(s2->handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            startup.dwFlags |= STARTF_USESTDHANDLES;
            startup.hStdInput = s0->handle;
            startup.hStdOutput = s1->handle;
            startup.hStdError = s2->handle;
        }
    }
    if (!CreateProcessW(path, cmd, 0, 0, TRUE, CREATE_UNICODE_ENVIRONMENT,
                        envp ? environment : 0, 0, &startup, &process)) {
        r = nt_last_error();
        VirtualFree(cmd, 0, MEM_RELEASE);
        VirtualFree(environment, 0, MEM_RELEASE);
        return r;
    }
    CloseHandle(process.hThread);
    child_add(process.dwProcessId, process.hProcess); /* pro wait4/kill */
    VirtualFree(cmd, 0, MEM_RELEASE);
    VirtualFree(environment, 0, MEM_RELEASE);
    return (nt_sc_t)process.dwProcessId; /* devolve o pid, NÃO sai */
}

nt_sc_t nt_sys_wait4(nt_sc_t pid_arg, nt_sc_t status_arg, nt_sc_t options, nt_sc_t rusage)
{
    int *status = (int *)(uintptr_t)status_arg;
    long pid = (long)pid_arg;
    HANDLE handles[MAXIMUM_WAIT_OBJECTS];
    DWORD pids[MAXIMUM_WAIT_OBJECTS];
    int count = 0, i, idx;
    DWORD wait, code = 0;
    (void)rusage;
    if (options & ~1L) return -NT_EINVAL; /* só WNOHANG */

    /* junta os filhos que casam: pid>0 = aquele; pid<=0 = qualquer um */
    AcquireSRWLockExclusive(&g_children_lock);
    for (i = 0; i < NT_MAX_CHILDREN && count < MAXIMUM_WAIT_OBJECTS; ++i) {
        if (!g_children[i].handle) continue;
        if (pid > 0 && g_children[i].pid != (DWORD)pid) continue;
        handles[count] = g_children[i].handle;
        pids[count] = g_children[i].pid;
        ++count;
    }
    ReleaseSRWLockExclusive(&g_children_lock);

    if (count == 0) return -NT_ECHILD; /* nenhum filho a esperar */

    wait = WaitForMultipleObjects((DWORD)count, handles, FALSE,
                                  (options & 1) ? 0 : INFINITE);
    if (wait == WAIT_TIMEOUT) return 0; /* WNOHANG: ninguém terminou */
    if (wait >= WAIT_OBJECT_0 + (DWORD)count) /* FAILED/ABANDONED/fora do range */
        return -NT_ECHILD;
    idx = (int)(wait - WAIT_OBJECT_0);
    GetExitCodeProcess(handles[idx], &code);
    if (status) *status = ((int)code & 0xff) << 8;

    /* reap: remove da tabela e fecha o handle */
    AcquireSRWLockExclusive(&g_children_lock);
    for (i = 0; i < NT_MAX_CHILDREN; ++i)
        if (g_children[i].handle == handles[idx] &&
            g_children[i].pid == pids[idx]) {
            CloseHandle(g_children[i].handle);
            g_children[i].handle = 0;
            break;
        }
    ReleaseSRWLockExclusive(&g_children_lock);
    return (nt_sc_t)pids[idx];
}

nt_sc_t nt_sys_kill(nt_sc_t pid, nt_sc_t sig)
{
    HANDLE process;
    if (pid <= 0) return -NT_ENOSYS;
    process = OpenProcess(sig ? PROCESS_TERMINATE | SYNCHRONIZE : SYNCHRONIZE,
                          FALSE, (DWORD)pid);
    if (!process) return nt_last_error();
    if (!sig) {
        CloseHandle(process);
        return 0;
    }
    if (sig != 9 && sig != 15) {
        CloseHandle(process);
        return -NT_ENOSYS;
    }
    if (!TerminateProcess(process, (UINT)(128 + sig))) {
        nt_sc_t r = nt_last_error();
        CloseHandle(process);
        return r;
    }
    CloseHandle(process);
    return 0;
}

nt_sc_t nt_sys_fork(void)
{
    static nt_clone_fn clone_fn;
    static int resolved;
    nt_clone_info info;
    LONG status;

    if (!resolved) {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        clone_fn = ntdll
            ? (nt_clone_fn)(uintptr_t)GetProcAddress(ntdll, "RtlCloneUserProcess")
            : 0;
        resolved = 1;
    }
    if (!clone_fn) return -NT_ENOSYS; /* Wine: sem fork nativo */

    nt_memset(&info, 0, sizeof info);
    /* todos os fds precisam ser herdáveis pra atravessar o clone */
    nt_fd_make_inheritable();
    status = clone_fn(NT_RTL_CLONE_CREATE_SUSPENDED | NT_RTL_CLONE_INHERIT_HANDLES,
                      0, 0, 0, &info);
    if (status == NT_STATUS_PROCESS_CLONED) {
        /* o processo clonado herda os handles mas não a conexão com o
         * conhost; reanexa ao console do pai e reassocia stdin/out/err,
         * senão o filho não consegue escrever (saía com 255 mudo). */
        FreeConsole();
        AttachConsole(ATTACH_PARENT_PROCESS);
        nt_fd_reattach_console();
        return 0; /* o filho da clonagem retorna 0, como o fork() do Unix */
    }
    if (status < 0)
        return nt_error_from_status(status);

    /* o pai retoma a thread do filho, guarda (pid,handle) pro wait4 e devolve
     * o pid (kill acha-o por pid via OpenProcess). */
    ResumeThread(info.ThreadHandle);
    CloseHandle(info.ThreadHandle);
    {
        DWORD pid = (DWORD)(uintptr_t)info.ClientId.UniqueProcess;
        child_add(pid, info.ProcessHandle);
        return (nt_sc_t)pid;
    }
}
