#include "nt/ntpriv.h"

/* fork() estilo Cygwin — NÃO usa RtlCloneUserProcess (aquele clone parcial não
 * consegue exec: o CreateProcessW crasha nele). Aqui o filho é um processo
 * NORMAL: CreateProcessW(a si mesmo, SUSPENSO) => crt0 do filho nunca roda; o
 * pai copia sua memória gravável pro filho no MESMO endereço virtual
 * (VirtualAllocEx + WriteProcessMemory) e transplanta o contexto do ponto do
 * fork() (RtlCaptureContext + SetThreadContext), fazendo o filho retomar ali
 * retornando 0. Como é um processo completo (CSR/loader íntegros), o filho pode
 * exec, ler, tudo. Bônus: só usa Win32 padrão => funciona no Wine também.
 *
 * Requisitos: (1) o thread pointer da musl é um global (ver __set_thread_area),
 * então a cópia de memória já o leva; (2) o exe é linkado com base fixa
 * (--disable-dynamicbase) para carregar no MESMO VA no pai e no filho — senão o
 * ctx.Rip e o .data copiado cairiam no lugar errado. */

/* ---- tabela de processos filhos: o fork registra (pid,handle) aqui e o
 * wait4 usa — inclusive waitpid(-1) (esperar QUALQUER filho), que o ash
 * precisa para coordenar os estágios de um pipeline. ---- */
#define NT_MAX_CHILDREN 256
static struct { DWORD pid; HANDLE handle; } g_children[NT_MAX_CHILDREN];
static SRWLOCK g_children_lock = SRWLOCK_INIT;

/* audit #83: devolve 0 se a tabela estava cheia (o CALLER trata o handle:
 * mata o filho e falha o spawn), em vez de fechar o handle e deixar um pid
 * que o waitpid nunca poderia aguardar (ECHILD). */
static int child_add(DWORD pid, HANDLE h)
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
    return i < NT_MAX_CHILDREN;
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
    if (!child_add(process.dwProcessId, process.hProcess)) { /* pro wait4/kill */
        /* audit #83: sem slot, o pid nao seria aguardavel -> mata o filho e
         * falha em vez de devolver um pid orfao */
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        VirtualFree(cmd, 0, MEM_RELEASE);
        VirtualFree(environment, 0, MEM_RELEASE);
        return -NT_EAGAIN;
    }
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
    /* kill(self, sig): entrega local — marca pendente e o handler roda no
     * retorno desta syscall (então raise()/abort() funcionam). */
    if ((DWORD)pid == GetCurrentProcessId())
        return sig ? nt_signal_post_local((int)sig) : 0;
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

/* O filho já foi SUSPENSO pelo chamador. Copia as regiões graváveis do pai:
 *  - regiões com MEM_TOP_DOWN (heap/brk/mmap/argv) estão em VA alta, LIVRE no
 *    filho -> VirtualAllocEx + copia limpo;
 *  - a STACK (contém rsp) e o .data/.bss do EXE colidem (VA baixa/base fixa),
 *    mas são SEGUROS de sobrescrever: o filho está suspenso e será redirecionado
 *    pra stack, e o musl init do filho não rodou (o .data dele é só o inicial);
 *  - TEB/PEB nunca (identidade do filho); outras colisões (heap/loader do filho)
 *    são puladas — com MEM_TOP_DOWN não devem conter memória nossa. */
static int nt_fork_copy_memory(HANDLE child, HMODULE exe, void *rsp)
{
    MEMORY_BASIC_INFORMATION mbi;
    unsigned char *addr = 0;
    int n = 0, copyfail = 0;   /* copyfail = escrita de regiao NOSSA falhou (#79) */
    void *teb = (void *)NtCurrentTeb();
    void *peb = (void *)__readgsqword(0x60); /* TEB->ProcessEnvironmentBlock */
    while (VirtualQuery(addr, &mbi, sizeof mbi)) {
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD)) {
            unsigned char *b = mbi.BaseAddress;
            unsigned char *e = b + mbi.RegionSize;
            DWORD base = mbi.Protect & 0xffU;
            int writable = base == PAGE_READWRITE || base == PAGE_WRITECOPY ||
                           base == PAGE_EXECUTE_READWRITE ||
                           base == PAGE_EXECUTE_WRITECOPY;
            int is_exe = mbi.Type == MEM_IMAGE &&
                         mbi.AllocationBase == (void *)exe;
            int mine = mbi.Type == MEM_PRIVATE || is_exe;
            int is_teb_peb = ((unsigned char *)teb >= b && (unsigned char *)teb < e) ||
                             ((unsigned char *)peb >= b && (unsigned char *)peb < e);
            int has_rsp = (unsigned char *)rsp >= b && (unsigned char *)rsp < e;
            if (writable && mine && !is_teb_peb) {
                SIZE_T wrote;
                DWORD old;
                if (VirtualAllocEx(child, b, mbi.RegionSize,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
                    /* VA livre no filho: cópia limpa. audit #79: checa a escrita
                     * — antes contava ++n mesmo com WriteProcessMemory falho,
                     * deixando a regiao alocada mas com lixo no filho. */
                    if (WriteProcessMemory(child, b, b, mbi.RegionSize, &wrote) &&
                        wrote == mbi.RegionSize) {
                        VirtualProtectEx(child, b, mbi.RegionSize, base, &old);
                        ++n;
                    } else {
                        ++copyfail;
                    }
                } else if (is_exe || has_rsp) {
                    /* colisão SEGURA (stack / .data do exe): sobrescreve. A
                     * stack do pai pode ter MAIS páginas committadas que a do
                     * filho — garante commit da faixa inteira antes de escrever. */
                    VirtualAllocEx(child, b, mbi.RegionSize, MEM_COMMIT,
                                   PAGE_READWRITE);
                    VirtualProtectEx(child, b, mbi.RegionSize, PAGE_READWRITE, &old);
                    if (WriteProcessMemory(child, b, b, mbi.RegionSize, &wrote) &&
                        wrote == mbi.RegionSize)
                        ++n;
                    else
                        ++copyfail;   /* #79: copia de regiao NOSSA falhou */
                    VirtualProtectEx(child, b, mbi.RegionSize, base, &old);
                } else {
                    /* colisão NÃO-segura (heap/loader do filho): esperado com a
                     * arena de VA fixa (nao contem memoria nossa) -> nao e' erro */
                }
            }
        }
        addr = (unsigned char *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.RegionSize == 0) break;
    }
    return (copyfail << 16) | (n & 0xffff);
}

nt_sc_t nt_sys_fork(void)
{
    WCHAR self[NT_PATH_CAP];
    WCHAR cmdline[48];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    CONTEXT ctx;
    SECURITY_ATTRIBUTES sa;
    HANDLE ev;
    DWORD parent_pid = GetCurrentProcessId();
    HMODULE exe = GetModuleHandleW(0);
    const char *hexd = "0123456789abcdef";
    uintptr_t hv;
    int i, k;

    if (!GetModuleFileNameW(0, self, NT_ARRAY_LEN(self)))
        return nt_last_error();
    /* todos os fds precisam ser herdáveis pra atravessar o fork */
    nt_fd_make_inheritable();

    /* evento herdável: o filho o sinaliza quando o loader terminar (IAT pronta) */
    sa.nLength = sizeof sa;
    sa.lpSecurityDescriptor = 0;
    sa.bInheritHandle = TRUE;
    ev = CreateEventW(&sa, TRUE, FALSE, 0);
    if (!ev) return nt_last_error();

    /* cmdline = "\x01FORK=<ev_hex>" — o marcador que o crt0 do filho detecta */
    cmdline[0] = 1; cmdline[1] = L'F'; cmdline[2] = L'O';
    cmdline[3] = L'R'; cmdline[4] = L'K'; cmdline[5] = L'=';
    hv = (uintptr_t)ev;
    k = 6;
    for (i = (int)sizeof hv * 2 - 1; i >= 0; --i)
        cmdline[k++] = (WCHAR)(unsigned char)hexd[(hv >> (i * 4)) & 0xf];
    cmdline[k] = 0;

    nt_memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    nt_memset(&pi, 0, sizeof pi);
    /* NÃO suspenso: o loader do filho roda (resolve a IAT, inicia DLLs); aí o
     * crt0 detecta o marcador, sinaliza 'ev' e fica girando. */
    if (!CreateProcessW(self, cmdline, 0, 0, TRUE, 0, 0, 0, &si, &pi)) {
        nt_sc_t r = nt_last_error();
        CloseHandle(ev);
        return r;
    }
    /* espera o filho terminar o loader (imports resolvidos) e sinalizar.
     * Timeout defensivo: se o filho não sinalizar (bug), não trava o pai. */
    if (WaitForSingleObject(ev, 10000) != WAIT_OBJECT_0) {
        CloseHandle(ev);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -NT_EAGAIN;
    }
    CloseHandle(ev);

    nt_memset(&ctx, 0, sizeof ctx);
    RtlCaptureContext(&ctx);
    /* ----- ponto de retomada: o PAI cai aqui já; o FILHO cai aqui depois, via
     * SetThreadContext. Distingue pelo pid (o filho tem outro). ----- */
    if (GetCurrentProcessId() != parent_pid) {
        return 0; /* ===== FILHO ===== retorna 0, como o fork() do Unix */
    }

    /* ===== PAI ===== suspende o filho (que está girando em user-mode no crt0,
     * com a IAT já resolvida), copia a memória (sobrescreve a stack dele com a
     * do pai — seguro pois está parado e vai ser redirecionado), transplanta o
     * contexto do ponto do fork() e resume. */
    SuspendThread(pi.hThread);
    /* audit #79: se alguma regiao NOSSA nao copiou, o filho retomaria com
     * memoria parcial (crash/corrupcao) — aborta em vez de resumir. Colisoes
     * benignas (heap/loader do filho) nao contam como falha. */
    if ((nt_fork_copy_memory(pi.hProcess, exe, (void *)(uintptr_t)ctx.Rsp) >> 16) != 0) {
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return -NT_ENOMEM;
    }
    ctx.ContextFlags = CONTEXT_FULL;
    if (!SetThreadContext(pi.hThread, &ctx)) {
        nt_sc_t r = nt_last_error();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return r;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    if (!child_add(pi.dwProcessId, pi.hProcess)) { /* pro wait4/kill */
        TerminateProcess(pi.hProcess, 1);          /* audit #83: pid orfao */
        CloseHandle(pi.hProcess);
        return -NT_EAGAIN;
    }
    return (nt_sc_t)pi.dwProcessId;
}
