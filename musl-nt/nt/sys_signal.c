#include "nt/ntpriv.h"

struct nt_kernel_sigaction {
    uintptr_t handler;
    uint64_t flags;
    uintptr_t restorer;
    uint64_t mask;
};

/* handler==0 => SIG_DFL, ==1 => SIG_IGN, senão ponteiro de função (ABI SysV). */
#define NT_SIG_DFL 0
#define NT_SIG_IGN 1

#define NT_SIGINT   2
#define NT_SIGKILL  9
#define NT_SIGPIPE 13
#define NT_SIGCHLD 17
#define NT_SIGCONT 18
#define NT_SIGSTOP 19
#define NT_SIGURG  23
#define NT_SIGWINCH 28

static struct nt_kernel_sigaction actions[65];
static uint64_t blocked_mask;
static SRWLOCK signal_lock = SRWLOCK_INIT;

/* ---- entrega de sinais ---- */
/* bitmask 64-bit (bit `sig`) de sinais pendentes. 64-bit é obrigatório: LONG é
 * 32-bit no Windows e `1 << sig` estouraria para os sinais em tempo real. */
static volatile LONGLONG g_pending;
static HANDLE g_main_thread;         /* handle real da thread principal */

static void post_signal(int sig)
{
    InterlockedOr64(&g_pending, (LONGLONG)(1ULL << sig));
}

/* kill(self, sig): marca pendente; entregue no próximo ponto seguro (syscall). */
int nt_signal_post_local(int sig)
{
    if (sig <= 0 || sig >= 65) return -NT_EINVAL;
    post_signal(sig);
    return 0;
}

/* Handler de controle do console (roda numa thread separada do SO). Só decide;
 * a execução do handler Unix acontece na thread principal, em nt_deliver. */
static BOOL WINAPI console_ctrl_handler(DWORD type)
{
    uintptr_t h;
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT) return FALSE;
    h = actions[NT_SIGINT].handler;
    if (h == NT_SIG_DFL) return FALSE;   /* default: o SO termina o processo */
    if (h == NT_SIG_IGN) return TRUE;    /* ignora: sobrevive, não faz nada */
    post_signal(NT_SIGINT);              /* handler: sobrevive e entrega depois */
    if (g_main_thread) CancelSynchronousIo(g_main_thread); /* acorda read/wait bloqueado */
    return TRUE;
}

/* SIGWINCH cross-process: o dispd (master) faz SetEvent num evento nomeado a cada
 * resize; esta thread espera nele e posta SIGWINCH local — exatamente como o
 * console_ctrl_handler faz com SIGINT. Referencia: um PTY real sinaliza o
 * foreground process group no TIOCSWINSZ. */
static DWORD WINAPI winch_watcher(void *arg)
{
    HANDLE ev = (HANDLE)arg;
    for (;;) {
        if (WaitForSingleObject(ev, INFINITE) != WAIT_OBJECT_0)
            break;
        post_signal(NT_SIGWINCH);
        if (g_main_thread) CancelSynchronousIo(g_main_thread);   /* acorda read bloqueado */
    }
    return 0;
}

void nt_install_signals(void)
{
    HANDLE real = 0;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                    GetCurrentProcess(), &real, 0, FALSE, DUPLICATE_SAME_ACCESS);
    g_main_thread = real;
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    char wn[64];
    if (GetEnvironmentVariableA("NTU_PTY_WINCH", wn, sizeof wn) > 0) {
        HANDLE ev = OpenEventA(SYNCHRONIZE, FALSE, wn);
        if (ev) {
            HANDLE th = CreateThread(NULL, 0, winch_watcher, ev, 0, NULL);
            if (th) CloseHandle(th); else CloseHandle(ev);
        }
    }
}

/* Entrega os sinais pendentes na THREAD ATUAL (chamado após cada syscall pelo
 * __nt_syscall). Roda os handlers Unix aqui, no contexto principal. */
void nt_deliver_pending_signals(void)
{
    LONGLONG pending;
    int sig;
    if (!g_pending) return; /* fast path */
    pending = InterlockedExchange64(&g_pending, 0);
    for (sig = 1; sig < 64; ++sig) {
        uintptr_t h;
        if (!(pending & (1ULL << sig))) continue;
        if (blocked_mask & (1ULL << (sig - 1))) { /* bloqueado: continua pendente */
            post_signal(sig);
            continue;
        }
        h = actions[sig].handler;
        if (h == NT_SIG_IGN) continue;
        if (h == NT_SIG_DFL) {
            /* ação default: ignora SIGCHLD/SIGURG/SIGWINCH/SIGCONT; senão termina */
            if (sig == NT_SIGCHLD || sig == NT_SIGURG ||
                sig == NT_SIGWINCH || sig == NT_SIGCONT)
                continue;
            ExitProcess((UINT)(128 + sig));
        } else {
            /* chama o handler com ABI SysV (é código musl/busybox) */
            ((void(__attribute__((sysv_abi)) *)(int))h)(sig);
        }
    }
}

nt_sc_t nt_sys_rt_sigaction(nt_sc_t sig, nt_sc_t act_arg, nt_sc_t old_arg, nt_sc_t size)
{
    const struct nt_kernel_sigaction *act = (const void *)(uintptr_t)act_arg;
    struct nt_kernel_sigaction *old = (void *)(uintptr_t)old_arg;
    if (size != 8) return -NT_EINVAL;
    if (sig <= 0 || sig >= (nt_sc_t)NT_ARRAY_LEN(actions) || sig == 9 || sig == 19)
        return -NT_EINVAL;
    AcquireSRWLockExclusive(&signal_lock);
    if (old) *old = actions[sig];
    if (act) actions[sig] = *act;
    ReleaseSRWLockExclusive(&signal_lock);
    return 0;
}

nt_sc_t nt_sys_rt_sigprocmask(nt_sc_t how, nt_sc_t set_arg, nt_sc_t old_arg, nt_sc_t size)
{
    const uint64_t *set = (const void *)(uintptr_t)set_arg;
    uint64_t *old = (void *)(uintptr_t)old_arg;
    if (size != 8) return -NT_EINVAL;
    if (how < 0 || how > 2) return -NT_EINVAL;
    AcquireSRWLockExclusive(&signal_lock);
    if (old) *old = blocked_mask;
    if (set) {
        uint64_t value = *set;
        value &= ~(1ULL << (9 - 1));
        value &= ~(1ULL << (19 - 1));
        if (how == 0) blocked_mask |= value;
        else if (how == 1) blocked_mask &= ~value;
        else blocked_mask = value;
    }
    ReleaseSRWLockExclusive(&signal_lock);
    return 0;
}
