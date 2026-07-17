#include "nt/ntpriv.h"

struct nt_kernel_sigaction {
    uintptr_t handler;
    uint64_t flags;
    uintptr_t restorer;
    uint64_t mask;
};

static struct nt_kernel_sigaction actions[65];
static uint64_t blocked_mask;
static SRWLOCK signal_lock = SRWLOCK_INIT;

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
